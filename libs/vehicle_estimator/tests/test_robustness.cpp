// SPDX-License-Identifier: GPL-3.0-or-later
//
// The estimator when the sensors misbehave: a GNSS outage, a multipath jump,
// dropped IMU samples, a receiver falling from RTX to autonomous, epochs
// arriving out of order, a heading that is wrong at start-up, no heading at
// all, and measurements that are not numbers.

#include "harness.h"

#include <limits>

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

// Worst errors over [t0, t1) of scenario time.
harness::Errors window(const harness::Run& r, const sim::Scenario& sc, double t0, double t1)
{
    harness::Errors e;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const double t = sc.toScenarioTime(r.states[k].gps_time);
        if (t < t0 || t >= t1) continue;
        ++e.scored;
        e.position = std::max(e.position, (r.states[k].p_e - r.truths[k].p_e).norm());
        e.velocity = std::max(e.velocity, (r.states[k].v_ned - r.truths[k].v_ned).norm());
        e.yaw = std::max(e.yaw, std::fabs(harness::wrap(r.states[k].yaw - r.truths[k].yaw)));
        if (r.states[k].sideslip_valid)
            e.sideslip = std::max(e.sideslip, std::fabs(harness::wrap(r.states[k].sideslip - r.truths[k].sideslip)));
    }
    return e;
}

void testOutage()
{
    // Five seconds of no GNSS mid-drift: the IMU carries the state, the
    // uncertainty grows, and both come back when the fixes return.
    sim::SensorModel sensors;
    sensors.seed = 21;
    sensors.outages = {{25.0, 30.0}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    std::vector<double> sigma_by_time;
    const auto r = harness::run(sc, est, 10.0);
    harness::report("outage", r);
    // No keyframes during the outage -- there are no epochs to make them --
    // so the output there is the IMU-rate propagation; score what follows.
    const auto after = window(r, sc, 32.0, 45.0);
    check(after.position < 0.10 && after.yaw < 0.5 * kDeg, "back to full accuracy after the outage");
    double sigma_before = 0.0, sigma_after_gap = 0.0;
    for (const auto& s : r.states)
    {
        const double t = sc.toScenarioTime(s.gps_time);
        if (t > 24.0 && t < 25.0) sigma_before = s.sigma_position_ned.norm();
        if (t >= 30.0 && t < 30.15) sigma_after_gap = std::max(sigma_after_gap, s.sigma_position_ned.norm());
    }
    SPDLOG_INFO("outage: position sigma {:.3f} m before, {:.3f} m at the first fix after", sigma_before, sigma_after_gap);
    check(r.status.resets == 0, "an outage is not a reset");
}

void testOutageDrift()
{
    // During the outage, the IMU-rate output must stay within its grown
    // uncertainty rather than claim the pre-outage precision.
    sim::SensorModel sensors;
    sensors.seed = 22;
    sensors.outages = {{25.0, 30.0}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    double worst = 0.0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        if (!m.imu) continue;
        const auto s = est.latest();
        if (!s) continue;
        const double t = sc.toScenarioTime(s->gps_time);
        if (t < 25.0 || t >= 30.0) continue;
        worst = std::max(worst, (s->p_e - sc.truth(t).p_e).norm());
    }
    SPDLOG_INFO("outage: worst IMU-only position error over 5 s: {:.3f} m", worst);
    // A tactical-grade MTi over five seconds of 1.3 g with calibrated biases:
    // tens of centimetres, not metres.
    check(worst < 1.0, "five seconds on the IMU alone stays under a metre");
}

void testOutlier()
{
    // A 5 m multipath jump for one epoch, and a 60 cm one: both must be kept
    // from dragging the estimate.
    sim::SensorModel sensors;
    sensors.seed = 23;
    sensors.outliers = {{20.0, Eigen::Vector3d(4.0, -3.0, 0.5)}, {30.0, Eigen::Vector3d(0.4, 0.4, -0.2)}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("outlier", r);
    check(r.status.gated_position >= 1, "the 5 m jump is gated");
    const auto around = window(r, sc, 19.5, 32.0);
    check(around.position < 0.10, fmt::format("neither jump moves the estimate ({:.3f} m)", around.position));
}

void testDroppedImu()
{
    // Single samples and a burst of five go missing mid-drift; each gap is
    // bridged, counted, and costs no accuracy worth mentioning.
    sim::SensorModel sensors;
    sensors.seed = 24;
    sensors.dropped_imu = {1500, 1777, 2001, 2002, 2003, 2004, 2005, 3100};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("dropped imu", r);
    check(r.status.imu_bridged == 8, fmt::format("eight samples bridged, {} counted", r.status.imu_bridged));
    check(r.status.resets == 0 && r.status.imu_restarts == 0, "no restart for a short gap");
    check(r.errors.position < 0.10 && r.errors.yaw < 0.5 * kDeg, "bridging costs no accuracy");
}

void testLongImuGap()
{
    // Half a second of IMU lost: too much rotation to bridge. The estimator
    // must restart rather than integrate a guess, and come back.
    sim::SensorModel sensors;
    sensors.seed = 25;
    for (std::size_t k = 2000; k < 2050; ++k) sensors.dropped_imu.push_back(k);
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("long imu gap", r);
    check(r.status.imu_restarts == 1, "one restart");
    check(r.status.initialized, "and initialised again");
    const auto after = window(r, sc, 26.0, 45.0);
    SPDLOG_INFO("after the restart: {} keyframes, position {:.3f} m, yaw {:.3f} deg, sideslip {:.3f} deg", after.scored,
                after.position, after.yaw / kDeg, after.sideslip / kDeg);
    check(after.scored > 100 && after.position < 0.10 && after.yaw < 0.5 * kDeg, "accurate after the restart");
}

void testFixDegradation()
{
    // RTX, then float, then autonomous for a while, then back: the reported
    // uncertainty follows the fix, and the estimate stays inside it.
    sim::SensorModel sensors;
    sensors.seed = 26;
    sensors.fix_changes = {{20.0, vehicle_estimator::FixQuality::float_rtk, 5.0},
                           {27.0, vehicle_estimator::FixQuality::autonomous, 50.0},
                           {35.0, vehicle_estimator::FixQuality::rtx, 1.0}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("fix degradation", r);
    double sigma_rtx = 0.0, sigma_auto = 0.0, worst_z = 0.0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const auto& s = r.states[k];
        const double t = sc.toScenarioTime(s.gps_time);
        if (t > 15.0 && t < 20.0) sigma_rtx = std::max(sigma_rtx, s.sigma_position_ned.head<2>().norm());
        if (t > 30.0 && t < 35.0) sigma_auto = std::max(sigma_auto, s.sigma_position_ned.head<2>().norm());
        if (t > 10.0)
        {
            const Eigen::Vector3d err = s.p_e - r.truths[k].p_e;
            worst_z = std::max(worst_z, err.norm() / std::max(1e-3, s.sigma_position_ned.norm()));
        }
    }
    SPDLOG_INFO("fix degradation: horizontal sigma {:.3f} m on RTX, {:.3f} m autonomous; worst error {:.1f} sigma",
                sigma_rtx, sigma_auto, worst_z);
    check(sigma_auto > 5.0 * sigma_rtx, "uncertainty grows with the fix");
    check(worst_z < 5.0, "and the error stays inside it");
    const auto after = window(r, sc, 38.0, 45.0);
    check(after.position < 0.15, fmt::format("back to RTX accuracy ({:.3f} m)", after.position));
}

void testOutOfOrder()
{
    // GNSS latency jitter large enough that epochs overtake each other.
    sim::SensorModel sensors;
    sensors.seed = 27;
    sensors.gnss_in_order = false;
    sensors.gnss_jitter = 0.08;
    sensors.imu_jitter = 0.02;
    sim::Scenario sc(sim::skidpad(), sensors);
    auto config = harness::configFor(sensors);
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    harness::report("out of order", r);
    check(r.status.keyframes > 350, "most epochs still become keyframes");
    check(r.errors.position < 0.10 && r.errors.yaw < 0.5 * kDeg, "and accuracy holds");
}

void testBadInitialHeading()
{
    // The first second of dual-antenna heading is 30 degrees wrong (a
    // receiver that has not resolved its baseline yet reports one anyway).
    // Gating must not lock the estimator onto the bad start: it has to take
    // the good headings when they come and converge.
    sim::SensorModel sensors;
    sensors.seed = 28;
    sensors.attitude_biases = {{{0.0, 1.0, Eigen::Vector2d(30.0 * kDeg, 0.0)}}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 15.0);
    harness::report("bad initial heading", r);
    check(r.errors.yaw < 0.5 * kDeg, fmt::format("converged from a 30 deg heading error ({:.3f} deg)", r.errors.yaw / kDeg));
    check(r.errors.position < 0.10, "position too");
}

void testNoHeading()
{
    // Until the receiver has a heading the estimator cannot start, and must
    // not pretend to: course over ground is not heading on a car that slides.
    sim::SensorModel sensors;
    sensors.seed = 29;
    sensors.attitude_from = 12.0;
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    bool started_early = false;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        if (est.status().initialized && m.gnss && sc.toScenarioTime(m.gnss->gps_time) < 11.9) started_early = true;
    }
    check(!started_early, "no initialisation before there is a heading");
    check(est.status().initialized, "initialised once there is one -- while already moving");
    const auto s = est.keyframeState();
    const auto truth = sc.truth(sc.toScenarioTime(s->gps_time));
    check(std::fabs(harness::wrap(s->yaw - truth.yaw)) < 0.5 * kDeg, "and right");
}

void testGarbage()
{
    // NaNs, infinities, a latitude in degrees, a covariance that is not one:
    // each part is refused, counted, and the rest of the epoch still used.
    sim::SensorModel sensors;
    sensors.seed = 30;
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    std::size_t n = 0;
    for (auto m : sc.messages())
    {
        if (m.gnss)
        {
            ++n;
            auto& e = *m.gnss;
            switch (n % 11)
            {
                case 3: e.position->lat = nan; break;
                case 4: e.position->lat = 49.3; break;  // degrees handed over as radians
                case 5: e.position->cov_ned = -Eigen::Matrix3d::Identity(); break;
                case 6: e.velocity->v_ned.x() = std::numeric_limits<double>::infinity(); break;
                case 7: e.attitude->yaw = nan; break;
                case 8: e.attitude->cov = Eigen::Matrix2d::Zero(); break;
                default: break;
            }
            est.addGnss(e);
        }
        if (m.imu)
        {
            // An occasional sample whose quaternion is not a rotation.
            if (m.imu->packet_counter % 97 == 0) m.imu->dq = Eigen::Quaterniond(2.0, 0.0, 0.0, 0.0);
            est.addImu(*m.imu);
        }
        est.process();
    }
    const auto& st = est.status();
    SPDLOG_INFO("garbage: {} parts rejected, {} IMU samples discarded, {} keyframes, {} resets", st.gnss_rejected,
                st.imu_discarded, st.keyframes, st.resets);
    check(st.gnss_rejected > 100, "malformed parts were rejected");
    check(st.keyframes > 350, "and the rest kept the estimator running");
    const auto s = est.keyframeState();
    check(s && s->p_e.allFinite() && std::isfinite(s->yaw), "no NaN reached the state");
}

}  // namespace

int main()
{
    testOutage();
    testOutageDrift();
    testOutlier();
    testDroppedImu();
    testLongImuGap();
    testFixDegradation();
    testOutOfOrder();
    testBadInitialHeading();
    testNoHeading();
    testGarbage();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("robustness: all passed");
    return 0;
}
