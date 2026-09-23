// SPDX-License-Identifier: GPL-3.0-or-later
//
// The estimator end to end on simulated drives, scored against truth at
// every keyframe once it has settled:
//
//   * a skidpad drift swinging between 3 and 26 degrees of slip,
//   * a figure of eight whose slip changes sign at the crossing,
//   * a spin past ninety degrees of slip,
//   * a car that never moves, which must stay put and stay level.
//
// The bounds are what an RTX receiver and an MTi-610 should support, not what
// the run happened to produce: a few centimetres, a few centimetres per
// second, a third of a degree.

#include "harness.h"

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

void expectAccurate(const harness::Run& r, const std::string& label, double sideslip_bound = 0.5 * kDeg)
{
    const auto& e = r.errors;
    check(r.status.resets == 0, label + ": no resets");
    check(e.scored > 200, label + ": keyframes were scored");
    check(e.position < 0.10, fmt::format("{}: position {:.3f} m", label, e.position));
    check(e.velocity < 0.10, fmt::format("{}: velocity {:.3f} m/s", label, e.velocity));
    check(e.yaw < 0.5 * kDeg, fmt::format("{}: yaw {:.3f} deg", label, e.yaw / kDeg));
    check(e.roll < 0.5 * kDeg, fmt::format("{}: roll {:.3f} deg", label, e.roll / kDeg));
    check(e.pitch < 0.5 * kDeg, fmt::format("{}: pitch {:.3f} deg", label, e.pitch / kDeg));
    check(e.sideslip < sideslip_bound, fmt::format("{}: sideslip {:.3f} deg", label, e.sideslip / kDeg));
    // The sideslip sigma the estimator reports has to mean something.
    check(e.worst_z_sideslip < 5.0, fmt::format("{}: worst sideslip error {:.1f} sigma", label, e.worst_z_sideslip));
}

void testSkidpad()
{
    sim::SensorModel sensors;
    sensors.reference_point = Eigen::Vector3d(-0.8, 0.0, -0.2);  // CG behind and below the IMU
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("skidpad", r);
    expectAccurate(r, "skidpad");
    check(r.errors.sideslip_scored > 250, "skidpad: sideslip was scored through the drift");
}

void testFigureEight()
{
    sim::SensorModel sensors;
    sensors.seed = 2;
    sim::Scenario sc(sim::figureEight(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("figure eight", r);
    expectAccurate(r, "figure eight");

    // Slip changes sign at the crossing; the estimate must follow it through
    // both signs, not just track its magnitude.
    std::size_t positive = 0, negative = 0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        if (!r.states[k].sideslip_valid) continue;
        if (r.truths[k].sideslip > 0.2 && r.states[k].sideslip > 0.15) ++positive;
        if (r.truths[k].sideslip < -0.2 && r.states[k].sideslip < -0.15) ++negative;
    }
    check(positive > 20 && negative > 20,
          fmt::format("figure eight: slip of both signs tracked ({} positive, {} negative)", positive, negative));
}

void testSpin()
{
    sim::SensorModel sensors;
    sensors.seed = 3;
    sim::SkidpadParams p;
    p.spin = true;
    p.drive = 30.0;
    sim::Scenario sc(sim::skidpad(p), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("spin", r);
    expectAccurate(r, "spin");
    double most = 0.0;
    for (const auto& t : r.truths) most = std::max(most, std::fabs(t.sideslip));
    check(most > 90.0 * kDeg, fmt::format("spin: slip reached {:.0f} deg", most / kDeg));
}

void testParked()
{
    sim::SensorModel sensors;
    sensors.seed = 4;
    sim::Scenario sc(sim::parked(60.0), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 5.0);
    harness::report("parked", r);
    check(r.status.resets == 0, "parked: no resets");
    check(r.errors.position < 0.05, fmt::format("parked: position {:.3f} m", r.errors.position));
    check(r.errors.velocity < 0.05, fmt::format("parked: velocity {:.3f} m/s", r.errors.velocity));
    check(r.errors.yaw < 0.3 * kDeg, fmt::format("parked: yaw {:.3f} deg", r.errors.yaw / kDeg));

    // Parked, an accelerometer bias and a tilt read identically: nothing
    // separates them until the car moves. So the level error is bounded by
    // the bias over g -- here 0.05 m/s^2, 0.3 deg -- and what must hold is
    // that the estimator KNOWS it: its roll and pitch sigma have to cover the
    // error, not claim the 0.01 deg the dual antenna gives yaw.
    const double bias_tilt = sensors.accel_bias.head<2>().norm() / 9.8;
    check(r.errors.roll < bias_tilt + 0.05 * kDeg && r.errors.pitch < bias_tilt + 0.05 * kDeg,
          fmt::format("parked: level to within the bias ({:.3f}, {:.3f} deg vs {:.3f})", r.errors.roll / kDeg,
                      r.errors.pitch / kDeg, bias_tilt / kDeg));
    double worst_z = 0.0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const auto& st = r.states[k];
        if (sc.toScenarioTime(st.gps_time) < 5.0) continue;
        worst_z = std::max(worst_z, std::fabs(harness::wrap(st.roll - r.truths[k].roll)) / st.sigma_attitude.x());
        worst_z = std::max(worst_z, std::fabs(harness::wrap(st.pitch - r.truths[k].pitch)) / st.sigma_attitude.y());
    }
    check(worst_z < 4.0, fmt::format("parked: roll/pitch error within its reported sigma ({:.1f} sigma)", worst_z));
    // Stopped, sideslip is undefined and must say so.
    bool any_valid = false;
    for (const auto& s : r.states) any_valid = any_valid || s.sideslip_valid;
    check(!any_valid, "parked: sideslip never claims to be valid");
    const auto s = est.latest();
    check(s && s->valid, "parked: the state is valid");
}

// The output between keyframes is the IMU carried forward from the newest
// keyframe; it must agree with truth at the IMU's time, not the keyframe's.
void testImuRateOutput()
{
    sim::SensorModel sensors;
    sensors.seed = 5;
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    double worst_yaw = 0.0, worst_pos = 0.0;
    std::size_t checked = 0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        if (!m.imu) continue;
        const auto s = est.latest();
        if (!s) continue;
        const double t = sc.toScenarioTime(s->gps_time);
        if (t < 12.0) continue;
        const auto truth = sc.truth(t);
        worst_yaw = std::max(worst_yaw, std::fabs(harness::wrap(s->yaw - truth.yaw)));
        worst_pos = std::max(worst_pos, (s->p_e - truth.p_e).norm());
        ++checked;
    }
    SPDLOG_INFO("IMU-rate output: {} samples, worst yaw {:.3f} deg, position {:.3f} m", checked, worst_yaw / kDeg,
                worst_pos);
    check(checked > 2500, "IMU-rate output was produced at IMU rate");
    check(worst_yaw < 0.5 * kDeg && worst_pos < 0.10, "IMU-rate output tracks truth between keyframes");
}

}  // namespace

int main()
{
    testSkidpad();
    testFigureEight();
    testSpin();
    testParked();
    testImuRateOutput();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("scenarios: all passed");
    return 0;
}
