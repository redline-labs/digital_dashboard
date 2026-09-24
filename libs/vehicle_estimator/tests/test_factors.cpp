// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measurement factors one at a time: zero at the truth, Jacobians that
// match finite differences through each variable's retraction, the robust
// loss bending as it should, and the traps -- yaw wrapping at north-by-south,
// the lever arm's rotational velocity, a covariance that is not one.

#include "vehicle_estimator/atmosphere.h"
#include "vehicle_estimator/factors.h"

#include "csym/geo/rot3.h"
#include "factor_graph/values.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <numbers>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

namespace factors = vehicle_estimator::factors;
using factor_graph::Key;
using factor_graph::symbol;
using factor_graph::Values;
using V3 = csym::Vector3<double>;

constexpr double kDeg = std::numbers::pi / 180.0;

V3 c(const Eigen::Vector3d& v)
{
    return V3{v.x(), v.y(), v.z()};
}

csym::Rot3<double> c(const Eigen::Quaterniond& q)
{
    return {q.x(), q.y(), q.z(), q.w()};
}

// Worst Jacobian column error against central differences, relative.
double jacobianError(const factor_graph::Factor& f, const Values& at)
{
    const auto lin = f.linearize(at);
    double worst = 0.0;
    const auto keys = f.keys();
    for (std::size_t var = 0; var < keys.size(); ++var)
    {
        const std::size_t n = at.variable(keys[var]).tangentDim();
        for (std::size_t col = 0; col < n; ++col)
        {
            // Not smaller: ECEF coordinates round at ~1e-9 m, which whitened
            // by a 2 cm sigma and divided by a 2e-5 step is noise of 3e-3.
            const double h = 1e-4;
            std::array<double, 9> d{};
            d[col] = h;
            Values p = at, m = at;
            p.variable(keys[var]).retract(std::span<const double>(d.data(), n));
            d[col] = -h;
            m.variable(keys[var]).retract(std::span<const double>(d.data(), n));
            const Eigen::VectorXd fd = (f.residual(p) - f.residual(m)) / (2 * h);
            const Eigen::VectorXd an = lin.jacobians[var].col(static_cast<Eigen::Index>(col));
            worst = std::max(worst, (fd - an).norm() / (1e-4 * an.norm() + 1e-2));
        }
    }
    return worst;
}

struct Setup
{
    Values values;
    Key R = symbol('R', 0), p = symbol('p', 0), v = symbol('v', 0), la = symbol('l', 0), bs = symbol('s', 0);
    Eigen::Quaterniond q;
    Eigen::Vector3d pos, vel, lever;
};

Setup setup()
{
    Setup s;
    s.q = Eigen::Quaterniond(Eigen::AngleAxisd(0.7, Eigen::Vector3d(0.2, -0.3, 0.9).normalized()));
    s.pos = Eigen::Vector3d(4.1e6, 6.2e5, 4.8e6);
    s.vel = Eigen::Vector3d(12.0, -3.0, 1.5);
    s.lever = Eigen::Vector3d(0.3, -0.1, 1.2);
    s.values.insert(s.R, c(s.q));
    s.values.insert(s.p, c(s.pos));
    s.values.insert(s.v, c(s.vel));
    s.values.insert(s.la, c(s.lever));
    s.values.insert(s.bs, csym::Vector2<double>{0.01, -0.02});
    return s;
}

void testGnssPosition()
{
    auto s = setup();
    const Eigen::Vector3d antenna = s.pos + s.q * s.lever;
    const Eigen::Matrix3d cov = Eigen::Vector3d(0.02, 0.02, 0.04).cwiseAbs2().asDiagonal();
    const auto f = factors::gnssPosition(s.R, s.p, s.la, antenna, cov, 3.0);
    check(f->residual(s.values).norm() < 1e-6, "position residual is zero at the truth");
    check(jacobianError(*f, s.values) < 1.0, "position Jacobians match finite differences");

    // Robust: 1 sigma off reads about 1; 100 sigma off reads far less than
    // 100 -- the loss is linear, so the whitened norm grows as a square root.
    const auto one = factors::gnssPosition(s.R, s.p, s.la, antenna + Eigen::Vector3d(0.02, 0, 0), cov, 3.0);
    const auto hundred = factors::gnssPosition(s.R, s.p, s.la, antenna + Eigen::Vector3d(2.0, 0, 0), cov, 3.0);
    const double r1 = one->residual(s.values).norm(), r100 = hundred->residual(s.values).norm();
    SPDLOG_INFO("robust position: 1 sigma reads {:.3f}, 100 sigma reads {:.2f}", r1, r100);
    // And with a huge transition point it is just the whitened residual --
    // no cancellation to zero.
    const auto plain = factors::gnssPosition(s.R, s.p, s.la, antenna + Eigen::Vector3d(0.02, 0, 0), cov, 1e9);
    check(std::fabs(plain->residual(s.values).norm() - 1.0) < 1e-6,
          fmt::format("a quadratic-only loss is exact: reads {:.12f}", plain->residual(s.values).norm()));
    check(std::fabs(r1 - 1.0) < 0.1, "quadratic near zero");
    check(r100 < 30.0 && r100 > 10.0, "linear far out: sqrt(2 delta x)");
}

void testGnssVelocity()
{
    auto s = setup();
    // The antenna moves faster than the IMU when the car turns: v + R (w x l).
    const Eigen::Vector3d w(0.1, -0.05, 0.9);
    const Eigen::Vector3d antenna_v = s.vel + s.q * w.cross(s.lever);
    const Eigen::Matrix3d cov = 0.0004 * Eigen::Matrix3d::Identity();
    const auto f = factors::gnssVelocity(s.R, s.v, s.la, antenna_v, cov, w, 3.0);
    check(f->residual(s.values).norm() < 1e-6, "velocity residual is zero at the truth, lever arm included");
    const auto wrong = factors::gnssVelocity(s.R, s.v, s.la, s.vel, cov, w, 3.0);
    // w x l here is 15 cm/s against a 2 cm/s sigma: 7.6 sigma, which the
    // robust loss reads as ~6.7.
    check(wrong->residual(s.values).norm() > 5.0, "leaving out the lever arm's rotation costs many sigmas");
    check(jacobianError(*f, s.values) < 1.0, "velocity Jacobians match finite differences");
}

void testDualAntenna()
{
    auto s = setup();
    const Eigen::Vector3d a1(0.3, 0.0, 1.2), a2(-1.2, 0.05, 1.25);
    const auto baseline = factors::Baseline::fromAntennas(a1, a2);
    check(std::fabs(baseline.b0.dot(baseline.u1)) < 1e-12 && std::fabs(baseline.b0.dot(baseline.u2)) < 1e-12 &&
              std::fabs(baseline.u1.dot(baseline.u2)) < 1e-12,
          "baseline and its tilt axes are perpendicular");

    const Eigen::Matrix3d R_n_e =
        Eigen::AngleAxisd(0.3, Eigen::Vector3d::UnitX()).toRotationMatrix() *
        Eigen::AngleAxisd(-1.1, Eigen::Vector3d::UnitZ()).toRotationMatrix();
    const Eigen::Vector2d bs(0.01, -0.02);
    const Eigen::Vector3d b_n = R_n_e * (s.q * baseline.direction(bs));
    const double yaw = std::atan2(b_n.y(), b_n.x());
    const double pitch = std::atan2(-b_n.z(), std::hypot(b_n.x(), b_n.y()));
    const Eigen::Matrix2d cov = Eigen::Vector2d(0.002 * 0.002, 0.004 * 0.004).asDiagonal();
    const auto f = factors::dualAntenna(s.R, s.bs, R_n_e, baseline, yaw, pitch, cov, 3.0);
    check(f->residual(s.values).norm() < 1e-6, "dual-antenna residual is zero at the truth");
    check(jacobianError(*f, s.values) < 1.0, "dual-antenna Jacobians match finite differences");

    // A measured yaw one full turn away is the same yaw.
    const auto turned = factors::dualAntenna(s.R, s.bs, R_n_e, baseline, yaw + 2.0 * std::numbers::pi, pitch, cov, 3.0);
    check(turned->residual(s.values).norm() < 1e-6, "yaw wraps");
    // A measurement a turn and 0.2 deg away reads 0.2 deg, not 359.8.
    const auto wrapped = factors::dualAntenna(s.R, s.bs, R_n_e, baseline, yaw + 0.2 * kDeg - 2.0 * std::numbers::pi,
                                              pitch, cov, 100.0);
    const double r = wrapped->residual(s.values)(0);
    check(std::fabs(std::fabs(r) - 0.2 * kDeg / 0.002) < 1e-3, fmt::format("a 0.2 deg yaw difference reads {:.3f} sigma", r));
    // Pitch sign: the baseline tilted up reads positive pitch.
    check(std::fabs(pitch - std::atan2(-b_n.z(), std::hypot(b_n.x(), b_n.y()))) < 1e-15, "pitch is up-positive");
}

void testCalibrationWalkAndPrior()
{
    // A mounting half a turn about x -- the default MTi installation -- so a
    // tangent convention that only works near identity shows up.
    const Eigen::Quaterniond m0 = Eigen::Quaterniond(Eigen::AngleAxisd(std::numbers::pi, Eigen::Vector3d::UnitX())) *
                                  Eigen::Quaterniond(Eigen::AngleAxisd(0.01, Eigen::Vector3d::UnitZ()));
    const auto k0 = vehicle_estimator::calibrationKeys(0), k1 = vehicle_estimator::calibrationKeys(1);
    Values v;
    v.insert(k0.mounting, c(m0));
    v.insert(k0.lever_arm, c(Eigen::Vector3d(0.3, 0.0, 1.2)));
    v.insert(k0.boresight, csym::Vector2<double>{0.01, -0.02});
    v.insert(k1.mounting, c(m0));
    v.insert(k1.lever_arm, c(Eigen::Vector3d(0.3, 0.0, 1.2)));
    v.insert(k1.boresight, csym::Vector2<double>{0.01, -0.02});

    // 1e-3 per sqrt(s) over 100 s allows 1e-2.
    v.insert(k0.magnetometer, csym::Vector<double, 9>{});
    v.insert(k0.barometer, csym::Vector2<double>{12.0, 0.2});
    v.insert(k1.magnetometer, csym::Vector<double, 9>{});
    v.insert(k1.barometer, csym::Vector2<double>{12.0, 0.2});
    // 1e-3 per sqrt(s) everywhere.
    const Eigen::Matrix<double, vehicle_estimator::kCalibrationDim, 1> q =
        Eigen::Matrix<double, vehicle_estimator::kCalibrationDim, 1>::Constant(1e-3);
    const auto walk = factors::calibrationWalk(k0, k1, 100.0, q);
    check(walk->residual(v).norm() < 1e-9, "the walk costs nothing when nothing moved");
    check(jacobianError(*walk, v) < 1.0, "walk Jacobians match finite differences");
    Values moved = v;
    moved.update(k1.lever_arm, c(Eigen::Vector3d(0.31, 0.0, 1.2)));
    check(std::fabs(walk->residual(moved).norm() - 1.0) < 1e-9, "1 cm after 100 s at 1 mm/sqrt(s) is one sigma");
    moved = v;
    moved.update(k1.mounting, c(m0 * Eigen::Quaterniond(Eigen::AngleAxisd(0.01, Eigen::Vector3d::UnitY()))));
    check(std::fabs(walk->residual(moved).norm() - 1.0) < 1e-6, "and so is 0.01 rad of mounting");
    check(jacobianError(*walk, moved) < 1.0, "walk Jacobians away from zero");

    // The prior, with the correlation a carried-over calibration has.
    vehicle_estimator::CalibrationSet prior;
    prior.mounting = m0;
    prior.lever_arm = Eigen::Vector3d(0.3, 0.0, 1.2);
    prior.boresight = Eigen::Vector2d(0.01, -0.02);
    prior.baro_offset = 12.0;
    prior.baro_airflow = 0.2;
    prior.cov = 1e-4 * vehicle_estimator::CalibrationCov::Identity();
    prior.cov(3, 6) = prior.cov(6, 3) = 0.5e-4;
    const auto p = factors::calibrationPrior(k0, prior);
    check(p->residual(v).norm() < 1e-9, "the prior is zero at its mean");
    check(jacobianError(*p, moved) < 1.0, "prior Jacobians match finite differences");
    moved = v;
    moved.update(k0.lever_arm, c(Eigen::Vector3d(0.31, 0.0, 1.2)));
    // Correlated at rho = 0.5 with a boresight that did not move: the lever
    // arm alone moving is less likely than its own sigma says, by
    // 1/sqrt(1 - rho^2). An independent-blocks prior would read exactly 1.
    check(std::fabs(p->residual(moved).norm() - 1.0 / std::sqrt(0.75)) < 1e-9,
          "the prior keeps the lever arm's correlation with the boresight");

    bool threw = false;
    try
    {
        auto zero = q;
        zero[4] = 0.0;
        factors::calibrationWalk(k0, k1, 1.0, zero);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    check(threw, "a walk density of zero is refused rather than divided by");
}

void testMountingFactors()
{
    // An IMU on its side (120 deg about (1,1,1)), not a half turn: a half
    // turn is its own inverse and would hide a mounting used backwards.
    const Eigen::Quaterniond m(Eigen::AngleAxisd(0.5 * std::numbers::pi, Eigen::Vector3d::UnitZ()) *
                               Eigen::AngleAxisd(0.5 * std::numbers::pi, Eigen::Vector3d::UnitX()));
    const Eigen::Matrix3d R_n_e = Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitY()).toRotationMatrix();
    // The car level and heading 30 deg: R_n_b = R_n_e R_e_i R_b_i^T.
    const Eigen::Matrix3d R_n_b = Eigen::AngleAxisd(0.52, Eigen::Vector3d::UnitZ()).toRotationMatrix();
    const Eigen::Quaterniond R_e_i(R_n_e.transpose() * R_n_b * m.toRotationMatrix());
    const double speed = 8.5;  // just over the gate
    const Eigen::Vector3d v_e = R_n_e.transpose() * R_n_b * Eigen::Vector3d(speed, 0.0, 0.0);

    const Key R = symbol('R', 0), v = symbol('v', 0), mk = symbol('m', 0);
    Values at;
    at.insert(R, c(R_e_i));
    at.insert(v, c(v_e));
    at.insert(mk, c(m));

    const auto straight = factors::straightDriving(R, v, mk, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero(),
                                                   0.5 * kDeg, 0.5 * kDeg, 3.0);
    check(straight->residual(at).norm() < 1e-6, "straight driving is zero for a body running true");
    check(jacobianError(*straight, at) < 1.0, "straight-driving Jacobians match finite differences");
    Values slid = at;
    slid.update(v, c(Eigen::Vector3d(R_n_e.transpose() * R_n_b *
                                     Eigen::Vector3d(speed * std::cos(0.5 * kDeg), speed * std::sin(0.5 * kDeg), 0.0))));
    const double one = straight->residual(slid).norm();
    check(std::fabs(one - 1.0) < 0.02, fmt::format("half a degree of slip reads one sigma ({:.3f})", one));
    check(jacobianError(*straight, slid) < 1.0, "and its Jacobians there");

    const auto level = factors::stationaryLevel(R, mk, R_n_e, 1.5 * kDeg, 3.0);
    check(level->residual(at).norm() < 1e-6, "stationary level is zero for a level body");
    check(jacobianError(*level, at) < 1.0, "stationary-level Jacobians match finite differences");
    Values tilted = at;
    const Eigen::Matrix3d R_n_b_tilt = R_n_b * Eigen::AngleAxisd(1.5 * kDeg, Eigen::Vector3d::UnitX()).toRotationMatrix();
    tilted.update(R, c(Eigen::Quaterniond(R_n_e.transpose() * R_n_b_tilt * m.toRotationMatrix())));
    check(std::fabs(level->residual(tilted).norm() - 1.0) < 0.02, "1.5 deg of roll reads one sigma");
}

void testBaroHeight()
{
    // A car at 812 m ellipsoidal height, doing 30 m/s, a sensor that sees 30%
    // of the dynamic pressure, and a 35 m offset: the pressure it reads is
    // exactly consistent, so the residual is zero there.
    namespace isa = vehicle_estimator::isa;
    const Eigen::Vector3d p0(4.2e6, 6.3e5, 4.7e6), up = p0.normalized();
    const double h0 = 812.0, offset = 35.0, airflow = 0.3;
    const Eigen::Vector3d v(20.0, 22.0, 0.5);
    const double H = h0 - offset;
    const double rho = isa::density(H);
    const double pressure = isa::pressure(H) + airflow * 0.5 * rho * v.squaredNorm();
    const Key kp = symbol('p', 0), kv = symbol('v', 0), ko = symbol('o', 0);
    Values at;
    at.insert(kp, c(p0));
    at.insert(kv, c(v));
    at.insert(ko, csym::Vector2<double>{offset, airflow});
    const auto f = factors::baroHeight(kp, kv, ko, pressure, p0, up, h0, rho, 0.5, 3.0);
    check(f->residual(at).norm() < 1e-6, fmt::format("barometric height is zero when consistent ({:.2e})", f->residual(at).norm()));
    check(jacobianError(*f, at) < 1.0, "barometric Jacobians match finite differences");
    Values higher = at;
    higher.update(kp, c(p0 + 0.5 * up));
    // One sigma, as the pseudo-Huber loss reads it (0.986 at delta 3).
    check(std::fabs(f->residual(higher).norm() - 1.0) < 0.02, "half a metre up is one sigma");
    check(jacobianError(*f, higher) < 1.0, "and its Jacobians there");
}

void testMagnetometerFactors()
{
    // The IMU on its side (not a half turn, whose inverse is itself) and a
    // calibration with every term non-zero, reading exactly the field it
    // should: zero residual, Jacobians that match finite differences.
    const Eigen::Quaterniond R_e_i(Eigen::AngleAxisd(0.7, Eigen::Vector3d(1, 1, 1).normalized()));
    const Eigen::Vector3d B_e(12000.0, -3000.0, 42000.0);
    const double F = B_e.norm();
    vehicle_estimator::CalibrationSet cal;
    cal.mag_hard_iron = Eigen::Vector3d(0.1, -0.2, 0.05);
    cal.mag_soft_iron << 0.03, -0.02, 0.04, 0.01, -0.015, 0.02;
    const Eigen::Vector3d m = cal.softIronMatrix() / F * (R_e_i.toRotationMatrix().transpose() * B_e) + cal.mag_hard_iron;
    const Key kR = symbol('R', 0), kk = symbol('k', 0);
    Values at;
    at.insert(kR, c(R_e_i));
    csym::Vector<double, 9> k;
    for (std::size_t i = 0; i < 9; ++i) k[i] = cal.magnetometer()[static_cast<Eigen::Index>(i)];
    at.insert(kk, k);
    const auto f = factors::magnetometer(kR, kk, m, B_e, F, 0.03, 3.0);
    check(f->residual(at).norm() < 1e-9, "magnetometer is zero at a consistent reading");
    check(jacobianError(*f, at) < 1.0, "magnetometer Jacobians match finite differences");

    // Heading only: the corrected field in a level frame, x magnetic north.
    const Eigen::Matrix3d R_n_e = Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitY()).toRotationMatrix();
    const Eigen::Vector3d north_down(0.5, 0.0, 0.85);
    const Eigen::Vector3d corrected_i = R_e_i.toRotationMatrix().transpose() * R_n_e.transpose() * north_down;
    const auto h = factors::magneticHeading(kR, corrected_i, R_n_e, 0.0, 3.0 * kDeg, 3.0);
    check(h->residual(at).norm() < 1e-9, "magnetic heading is zero pointing at magnetic north");
    check(jacobianError(*h, at) < 1.0, "magnetic heading Jacobians match finite differences");
    Values turned = at;
    turned.update(kR, c(Eigen::Quaterniond(Eigen::AngleAxisd(3.0 * kDeg, R_n_e.transpose().col(2))) * R_e_i));
    check(std::fabs(h->residual(turned).norm() - 1.0) < 0.02, "three degrees of yaw reads one sigma");
}

void testRefusals()
{
    bool threw = false;
    try
    {
        factors::sqrtInformation(-Eigen::Matrix3d::Identity());
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    check(threw, "a covariance that is not positive definite is refused");
    threw = false;
    try
    {
        factors::Baseline::fromAntennas(Eigen::Vector3d::Zero(), Eigen::Vector3d(0.01, 0.0, 0.0));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    check(threw, "a 1 cm baseline is refused");
}

}  // namespace

int main()
{
    testGnssPosition();
    testGnssVelocity();
    testDualAntenna();
    testCalibrationWalkAndPrior();
    testMountingFactors();
    testBaroHeight();
    testMagnetometerFactors();
    testRefusals();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("factors: all passed");
    return 0;
}
