// SPDX-License-Identifier: GPL-3.0-or-later
//
// The IMU model against truth that was built a different way.
//
// sim.cpp produces increments from inertial-frame kinematics; imu_model.h
// predicts in the rotating ECEF frame with Coriolis and gravity. If the two
// agree over a drift, a tumble and a parked car, the ECEF equations -- every
// sign, every earth-rate term -- are right. Then the factor built on them is
// checked the way the smoother will use it: residual at the truth, Jacobians
// against finite differences, and a solve that recovers a biased IMU.

#include "imu_preint/imu_factor.h"
#include "imu_preint/sim.h"

#include "factor_graph/optimizer.h"
#include "factor_graph/csym_factor.h"
#include "geodesy/wgs84.h"
#include "csym/geo/rot3.h"

#include "trajectories.h"

#include <spdlog/spdlog.h>

#include <cmath>
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

using imu_preint::DvFrame;
using imu_preint::NavState;
using imu_preint::Preintegrated;

const Eigen::Vector3d kOmega(0.0, 0.0, geodesy::wgs84::kOmegaIe);

NavState stateAt(const imu_preint::Trajectory& traj, double t)
{
    const auto s = traj.at(t);
    return {Eigen::Quaterniond(s.R_e_b), s.p_e, s.v_e};
}

Eigen::Vector3d gravityAt(const Eigen::Vector3d& p)
{
    const auto g = geodesy::normalGravityEcef(csym::Vector3<double>{p.x(), p.y(), p.z()});
    return {g[0], g[1], g[2]};
}

struct Errors
{
    double rot = 0.0, vel = 0.0, pos = 0.0;
};

Errors predictionError(const imu_preint::Trajectory& traj, double t0, double duration, DvFrame sim_frame,
                       DvFrame model_frame, const Eigen::Vector3d& omega = kOmega)
{
    const geodesy::NormalGravity g;
    const auto n = static_cast<std::size_t>(std::lround(duration * 100.0));
    const auto incs = imu_preint::simulateIncrements(traj, t0, 100.0, n, sim_frame, g);
    imu_preint::Preintegrator pre({}, model_frame);
    for (const auto& inc : incs) pre.integrate(inc);
    const NavState i = stateAt(traj, t0), j = stateAt(traj, t0 + duration);
    const NavState p = imu_preint::predict(i, {}, {}, pre.result(), gravityAt(i.p_e), omega);
    Errors e;
    e.rot = imu_preint::rotationVector(p.R_e_b.inverse() * j.R_e_b).norm();
    e.vel = (p.v_e - j.v_e).norm();
    e.pos = (p.p_e - j.p_e).norm();
    return e;
}

void testPrediction()
{
    const test_traj::Stationary parked;
    const test_traj::Skidpad drift;
    const test_traj::Tumble tumble;
    struct Case
    {
        const imu_preint::Trajectory* traj;
        const char* name;
    };
    for (const Case c : {Case{&parked, "parked"}, Case{&drift, "skidpad"}, Case{&tumble, "tumble"}})
    {
        for (const DvFrame frame : {DvFrame::start, DvFrame::end})
        {
            for (const double duration : {0.1, 1.0})
            {
                const Errors e = predictionError(*c.traj, 2.0, duration, frame, frame);
                SPDLOG_INFO("{} {} {:.1f} s: rotation {:.2g} rad, velocity {:.2g} m/s, position {:.2g} m", c.name,
                            frame == DvFrame::start ? "start" : "end", duration, e.rot, e.vel, e.pos);
                // What remains is discretisation inside a sample (second order
                // in the sample's rotation) and the omega^2 terms the model
                // drops -- orders of magnitude under the MTi's own noise over
                // the same interval (~4e-5 rad, ~2e-4 m/s at 0.1 s).
                const double scale = duration * duration;
                check(e.rot < 1e-9, fmt::format("{} rotation error {:.3g}", c.name, e.rot));
                check(e.vel < 2e-4 * duration, fmt::format("{} velocity error {:.3g}", c.name, e.vel));
                check(e.pos < 1e-4 * scale + 1e-6, fmt::format("{} position error {:.3g}", c.name, e.pos));
            }
        }
    }
}

// The settings and terms that are easy to get wrong each have to be visible.
void testWhatMatters()
{
    const test_traj::Stationary parked;
    const test_traj::Skidpad drift;

    // A parked gyro sees the earth turn; without the correction the model
    // thinks the car rotated by 15 deg/h.
    const Errors with = predictionError(parked, 0.0, 1.0, DvFrame::end, DvFrame::end);
    const Errors without = predictionError(parked, 0.0, 1.0, DvFrame::end, DvFrame::end, Eigen::Vector3d::Zero());
    check(with.rot < 1e-9 && without.rot > 5e-5,
          fmt::format("earth rate: {:.3g} rad with, {:.3g} without", with.rot, without.rot));

    // Choosing the wrong dv frame on a drift costs a systematic velocity
    // error far bigger than the model's own.
    const Errors right = predictionError(drift, 1.0, 1.0, DvFrame::end, DvFrame::end);
    const Errors wrong = predictionError(drift, 1.0, 1.0, DvFrame::end, DvFrame::start);
    check(wrong.vel > 50.0 * right.vel,
          fmt::format("dv frame: {:.3g} m/s right, {:.3g} m/s wrong", right.vel, wrong.vel));
    SPDLOG_INFO("wrong dv frame on the skidpad costs {:.3g} m/s per second", wrong.vel);
}

// ---- the factor --------------------------------------------------------------

struct Built
{
    factor_graph::Values truth;
    std::shared_ptr<const factor_graph::Factor> factor;
    imu_preint::KeyframeKeys i, j;
};

imu_preint::KeyframeKeys keysFor(std::uint64_t k)
{
    using factor_graph::symbol;
    return {symbol('R', k), symbol('p', k), symbol('v', k), symbol('g', k), symbol('a', k)};
}

csym::Rot3<double> toRot(const Eigen::Quaterniond& q)
{
    return {q.x(), q.y(), q.z(), q.w()};
}

csym::Vector3<double> toV(const Eigen::Vector3d& v)
{
    return {v.x(), v.y(), v.z()};
}

Built buildFactor(const imu_preint::Trajectory& traj, double t0, double duration, const Eigen::Vector3d& bg,
                  const Eigen::Vector3d& ba)
{
    const geodesy::NormalGravity g;
    // One sample before the interval, as the running estimator always has.
    auto incs = imu_preint::simulateIncrements(
        traj, t0 - 0.01, 100.0, static_cast<std::size_t>(std::lround(duration * 100)) + 1, DvFrame::end, g);
    // A biased sensor integrates the true rate plus the bias, so the bias adds
    // to the rotation vector -- not a rotation composed after the fact, which
    // differs by 1/2 theta x (b dt) per sample.
    for (auto& inc : incs)
    {
        inc.dq = imu_preint::fromRotationVector(imu_preint::rotationVector(inc.dq) + bg * inc.dt);
        inc.dv += ba * inc.dt;
    }
    imu_preint::Preintegrator pre({}, DvFrame::end);
    pre.integrate(incs.front());
    pre.reset(Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero());
    for (std::size_t k = 1; k < incs.size(); ++k) pre.integrate(incs[k]);

    Built b;
    b.i = keysFor(0);
    b.j = keysFor(1);
    const NavState si = stateAt(traj, t0), sj = stateAt(traj, t0 + duration);
    b.factor = imu_preint::makeImuFactor(b.i, b.j, pre.result(), gravityAt(si.p_e), kOmega);
    for (const auto& [keys, s] : {std::pair{b.i, si}, std::pair{b.j, sj}})
    {
        b.truth.insert(keys.R, toRot(s.R_e_b));
        b.truth.insert(keys.p, toV(s.p_e));
        b.truth.insert(keys.v, toV(s.v_e));
    }
    b.truth.insert(b.i.bg, toV(bg));
    b.truth.insert(b.i.ba, toV(ba));
    return b;
}

void testFactorAtTruth()
{
    const test_traj::Skidpad drift;
    const Built b = buildFactor(drift, 1.0, 0.1, Eigen::Vector3d(0.002, -0.001, 0.003), Eigen::Vector3d(0.05, -0.1, 0.02));
    check(factor_graph::factorProblem(*b.factor, b.truth).empty(), "IMU factor is well formed");
    const Eigen::VectorXd r = b.factor->residual(b.truth);
    SPDLOG_INFO("IMU factor whitened residual at truth: {:.3g}", r.cwiseAbs().maxCoeff());
    check(r.cwiseAbs().maxCoeff() < 0.05, fmt::format("residual at truth is noise-free: {:.3g}", r.cwiseAbs().maxCoeff()));

    // Jacobians against finite differences through each variable's retraction.
    const auto lin = b.factor->linearize(b.truth);
    const auto keys = b.factor->keys();
    double worst = 0.0;
    for (std::size_t var = 0; var < keys.size(); ++var)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            // Not smaller: an ECEF position rounds at ~1e-9 m, which after
            // whitening by a ~1e-5 m sigma is ~1e-4 per residual evaluation,
            // and a 2e-6 difference step turns that into noise of ~50 on
            // entries of ~1e5.
            const double h = 1e-4;
            std::array<double, 3> d{};
            d[c] = h;
            factor_graph::Values p = b.truth, m = b.truth;
            p.variable(keys[var]).retract(d);
            d[c] = -h;
            m.variable(keys[var]).retract(d);
            const Eigen::VectorXd fd = (b.factor->residual(p) - b.factor->residual(m)) / (2 * h);
            const Eigen::VectorXd an = lin.jacobians[var].col(static_cast<Eigen::Index>(c));
            // The ~0.5 absolute rounding floor above, plus 1e-4 relative.
            const double err = (fd - an).norm() / (1e-4 * an.norm() + 1.0);
            if (err > 1.0)
                SPDLOG_WARN("var {} col {}: |fd| {:.6g} |an| {:.6g} |diff| {:.3g}", var, c, fd.norm(), an.norm(),
                            (fd - an).norm());
            worst = std::max(worst, err);
        }
    }
    check(worst < 1.0, fmt::format("IMU factor Jacobians vs finite differences: {:.3g} of tolerance", worst));
}

// With both keyframes pinned by strong priors, one second of skidpad
// determines both biases: the rotation fixes the gyro, the velocity change
// the accelerometer.
constexpr auto kPriorV3 = [](auto x, auto mean, auto inv_sigma) { return (x - mean) * inv_sigma; };
constexpr auto kPriorRot = [](auto r, auto mean, auto inv_sigma, auto eps) {
    return csym::local_coordinates(mean, r, eps) * inv_sigma;
};
using PriorV3 = factor_graph::CsymFactor<kPriorV3, factor_graph::Vars<csym::Vector3<double>>,
                                         factor_graph::Params<csym::Vector3<double>, double>>;
using PriorRot = factor_graph::CsymFactor<kPriorRot, factor_graph::Vars<csym::Rot3<double>>,
                                          factor_graph::Params<csym::Rot3<double>, double, double>>;

void testBiasRecovery()
{
    const test_traj::Skidpad drift;
    const Eigen::Vector3d bg(0.004, -0.002, 0.003), ba(0.08, -0.05, 0.12);
    const Built b = buildFactor(drift, 0.5, 1.0, bg, ba);
    factor_graph::FactorList f{b.factor};
    for (const auto& keys : {b.i, b.j})
    {
        f.push_back(std::make_shared<PriorRot>("R", std::array<factor_graph::Key, 1>{keys.R},
                                               b.truth.at<csym::Rot3<double>>(keys.R), 1e6, factor_graph::kEpsilon));
        f.push_back(std::make_shared<PriorV3>("p", std::array<factor_graph::Key, 1>{keys.p},
                                              b.truth.at<csym::Vector3<double>>(keys.p), 1e4));
        f.push_back(std::make_shared<PriorV3>("v", std::array<factor_graph::Key, 1>{keys.v},
                                              b.truth.at<csym::Vector3<double>>(keys.v), 1e4));
    }
    factor_graph::Values start = b.truth;
    start.update(b.i.bg, csym::Vector3<double>{0.0, 0.0, 0.0});
    start.update(b.i.ba, csym::Vector3<double>{0.0, 0.0, 0.0});
    const auto report = factor_graph::optimize(f, start, {});
    check(report.converged, "bias solve converged: " + report.stop_reason);
    const auto gb = start.at<csym::Vector3<double>>(b.i.bg), ab = start.at<csym::Vector3<double>>(b.i.ba);
    const Eigen::Vector3d eg(gb[0] - bg.x(), gb[1] - bg.y(), gb[2] - bg.z());
    const Eigen::Vector3d ea(ab[0] - ba.x(), ab[1] - ba.y(), ab[2] - ba.z());
    SPDLOG_INFO("bias recovery: gyro error {:.3g} rad/s, accel error {:.3g} m/s^2", eg.norm(), ea.norm());
    check(eg.norm() < 1e-5, "gyro bias recovered");
    check(ea.norm() < 1e-3, "accel bias recovered");
}

}  // namespace

int main()
{
    testPrediction();
    testWhatMatters();
    testFactorAtTruth();
    testBiasRecovery();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("imu: all passed");
    return 0;
}
