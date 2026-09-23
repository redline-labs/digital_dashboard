// SPDX-License-Identifier: GPL-3.0-or-later
//
// The measurement factors one at a time: zero at the truth, Jacobians that
// match finite differences through each variable's retraction, the robust
// loss bending as it should, and the traps -- yaw wrapping at north-by-south,
// the lever arm's rotational velocity, a covariance that is not one.

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
            std::array<double, 3> d{};
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
    testRefusals();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("factors: all passed");
    return 0;
}
