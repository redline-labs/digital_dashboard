// SPDX-License-Identifier: GPL-3.0-or-later
//
// The preintegrator on its own: bias Jacobians against re-integration, the
// right Jacobian against its definition, splitting an increment, and every
// kind of increment it must refuse.

#include "imu_preint/preintegrator.h"
#include "imu_preint/sim.h"

#include "trajectories.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
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
using imu_preint::Increment;
using imu_preint::Preintegrated;
using imu_preint::Preintegrator;

Preintegrated integrateAll(const std::vector<Increment>& incs, DvFrame frame, const Eigen::Vector3d& bg,
                           const Eigen::Vector3d& ba)
{
    Preintegrator p({}, frame, bg, ba);
    for (const auto& inc : incs) p.integrate(inc);
    return p.result();
}

Eigen::Vector3d logMap(const Eigen::Matrix3d& R)
{
    return imu_preint::rotationVector(Eigen::Quaterniond(R));
}

// Every bias Jacobian, column by column, against central differences of a
// full re-integration. A sign slip in one of the five update lines shows up
// as one wrong block here.
void testBiasJacobians(DvFrame frame, const char* label)
{
    const test_traj::Skidpad traj;
    const geodesy::NormalGravity g;
    const auto incs = imu_preint::simulateIncrements(traj, 0.0, 100.0, 50, frame, g);
    const Eigen::Vector3d bg0(1e-3, -2e-3, 5e-4), ba0(0.02, -0.01, 0.03);
    const Preintegrated base = integrateAll(incs, frame, bg0, ba0);

    const double h = 1e-6;
    double worst = 0.0;
    for (int which = 0; which < 2; ++which)
    {
        for (int c = 0; c < 3; ++c)
        {
            Eigen::Vector3d d = Eigen::Vector3d::Zero();
            d[c] = h;
            const Preintegrated plus = which == 0 ? integrateAll(incs, frame, bg0 + d, ba0)
                                                  : integrateAll(incs, frame, bg0, ba0 + d);
            const Preintegrated minus = which == 0 ? integrateAll(incs, frame, bg0 - d, ba0)
                                                   : integrateAll(incs, frame, bg0, ba0 - d);
            const Eigen::Vector3d fd_R = (logMap(base.dR.transpose() * plus.dR) - logMap(base.dR.transpose() * minus.dR)) / (2 * h);
            const Eigen::Vector3d fd_v = (plus.dv - minus.dv) / (2 * h);
            const Eigen::Vector3d fd_p = (plus.dp - minus.dp) / (2 * h);
            const Eigen::Vector3d an_R = which == 0 ? Eigen::Vector3d(base.dR_dbg.col(c)) : Eigen::Vector3d::Zero();
            const Eigen::Vector3d an_v = which == 0 ? base.dv_dbg.col(c) : base.dv_dba.col(c);
            const Eigen::Vector3d an_p = which == 0 ? base.dp_dbg.col(c) : base.dp_dba.col(c);
            const double err = std::max({(fd_R - an_R).norm(), (fd_v - an_v).norm() / std::max(1.0, an_v.norm()),
                                         (fd_p - an_p).norm() / std::max(1.0, an_p.norm())});
            worst = std::max(worst, err);
            check(err < 1e-6, fmt::format("{} bias Jacobian {} axis {}: error {:.3g}", label, which == 0 ? "bg" : "ba",
                                          c, err));
        }
    }
    SPDLOG_INFO("{}: bias Jacobians worst error {:.3g}", label, worst);

    // And used as intended: correcting to a nearby bias without re-integrating
    // is accurate to second order in the change.
    const Eigen::Vector3d dbg(2e-4, -1e-4, 3e-4), dba(0.01, 0.005, -0.008);
    const Preintegrated moved = integrateAll(incs, frame, bg0 + dbg, ba0 + dba);
    const Eigen::Vector3d dv_first = base.dv + base.dv_dbg * dbg + base.dv_dba * dba;
    const double first_order = (dv_first - moved.dv).norm();
    const double zeroth_order = (base.dv - moved.dv).norm();
    check(first_order < 1e-3 * zeroth_order,
          fmt::format("{}: first-order bias correction {:.3g} vs none {:.3g}", label, first_order, zeroth_order));
}

// Splitting one increment and integrating both halves must land where the
// whole increment does; that is what happens at every keyframe that falls
// inside an IMU sample.
void testSplit(DvFrame frame, const char* label)
{
    const test_traj::Tumble traj;
    const geodesy::NormalGravity g;
    const auto incs = imu_preint::simulateIncrements(traj, 0.3, 100.0, 1, frame, g);
    const Increment& whole = incs[0];
    for (double f : {0.1, 0.5, 0.93})
    {
        const auto [a, b] = imu_preint::split(whole, f, frame);
        check(std::fabs(a.dt + b.dt - whole.dt) < 1e-15, "split durations add up");
        const Preintegrated one = integrateAll({whole}, frame, {}, {});
        const Preintegrated two = integrateAll({a, b}, frame, {}, {});
        const double rot = logMap(one.dR.transpose() * two.dR).norm();
        const double vel = (one.dv - two.dv).norm();
        // Within one sample the constant-rate assumption is the only error:
        // 1e-6 m/s under a 2 rad/s tumble, fifty times under one sample's
        // accelerometer noise.
        check(rot < 1e-12 && vel < 1e-5,
              fmt::format("{} split at {}: rotation {:.3g} rad, velocity {:.3g} m/s", label, f, rot, vel));
    }
    // A split exactly at the ends is the whole increment and nothing.
    const auto [none, all] = imu_preint::split(whole, 0.0, frame);
    check(none.dt == 0.0 && std::fabs(all.dt - whole.dt) < 1e-15, "split at 0");
}

void testRefusals()
{
    Preintegrator p({}, DvFrame::end);
    Increment good;
    good.dt = 0.01;
    good.dv = Eigen::Vector3d(0.0, 0.0, -0.098);
    check(p.integrate(good).empty(), "a good increment is accepted");
    const Preintegrated before = p.result();

    const auto refuse = [&](Increment inc, const std::string& what, double rot_var = 0.0) {
        const std::string e = p.integrate(inc, rot_var);
        check(!e.empty(), what + " is refused");
        check(p.result().samples == before.samples && p.result().dv == before.dv && p.result().dt == before.dt,
              what + " leaves the state untouched");
    };
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    Increment bad = good;
    bad.dt = 0.0;
    refuse(bad, "zero dt");
    bad.dt = -0.01;
    refuse(bad, "negative dt");
    bad.dt = nan;
    refuse(bad, "NaN dt");
    bad.dt = 5.0;
    refuse(bad, "a five-second increment");
    bad = good;
    bad.dv.x() = nan;
    refuse(bad, "NaN dv");
    bad = good;
    bad.dq = Eigen::Quaterniond(nan, 0, 0, 0);
    refuse(bad, "NaN dq");
    bad = good;
    bad.dq = Eigen::Quaterniond(1.1, 0, 0, 0);
    refuse(bad, "a quaternion 10% off unit length");
    refuse(good, "a negative extra variance", -1.0);

    // Slightly off unit (as a float round-trip leaves it) is renormalised.
    Increment close = good;
    close.dq = Eigen::Quaterniond(1.0005, 0.0, 0.0, 0.0);
    check(p.integrate(close).empty(), "a nearly-unit quaternion is accepted");
    check(std::fabs(Eigen::Quaterniond(p.result().dR).norm() - 1.0) < 1e-12, "and the result stays a rotation");
}

// A bridged sample carries the extra variance it was given.
void testBridge()
{
    Increment inc;
    inc.dt = 0.01;
    Preintegrator a({}, DvFrame::end), b({}, DvFrame::end);
    a.integrate(inc);
    b.integrate(inc, 1e-4, 1e-2, true);
    check(b.result().bridged == 1 && a.result().bridged == 0, "bridged samples are counted");
    check(b.result().cov(0, 0) > a.result().cov(0, 0) + 0.9e-4, "bridging inflates rotation variance");
    check(b.result().cov(3, 3) > a.result().cov(3, 3) + 0.9e-2, "bridging inflates velocity variance");
}

}  // namespace

int main()
{
    testBiasJacobians(DvFrame::start, "start frame");
    testBiasJacobians(DvFrame::end, "end frame");
    testSplit(DvFrame::start, "start frame");
    testSplit(DvFrame::end, "end frame");
    testRefusals();
    testBridge();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("preintegrator: all passed");
    return 0;
}
