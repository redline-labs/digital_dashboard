// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the estimator learns about the car while it drives: the IMU biases,
// the lever arm to the primary antenna, and the boresight of the antenna
// baseline -- and what it cannot learn, which must stay at its prior.

#include "harness.h"

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

void testBiases()
{
    sim::SensorModel sensors;
    sensors.seed = 11;
    sensors.gyro_bias = Eigen::Vector3d(0.003, -0.002, 0.0025);
    sensors.accel_bias = Eigen::Vector3d(0.06, -0.04, 0.03);
    sim::Scenario sc(sim::figureEight(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("biases", r);
    const Eigen::Vector3d eg = r.status.gyro_bias - sensors.gyro_bias;
    const Eigen::Vector3d ea = r.status.accel_bias - sensors.accel_bias;
    SPDLOG_INFO("bias errors: gyro [{:.2e} {:.2e} {:.2e}] rad/s, accel [{:.3f} {:.3f} {:.3f}] m/s^2", eg.x(), eg.y(),
                eg.z(), ea.x(), ea.y(), ea.z());
    // Turn-on biases of 0.17 deg/s and 6 mg, found to a twentieth.
    check(eg.norm() < 2e-4, "gyro bias recovered");
    check(ea.norm() < 0.01, "accelerometer bias recovered");
}

void testLeverArm()
{
    // The lever arm as measured with a tape is 8 cm off in x and 5 cm in y;
    // turning makes it observable (a lever arm on a rotating body moves the
    // antenna relative to the IMU), so the estimate must walk to the truth.
    sim::SensorModel sensors;
    sensors.seed = 12;
    auto config = harness::configFor(sensors);
    config.lever_arm = sensors.lever_arm + Eigen::Vector3d(0.08, -0.05, 0.0);
    config.lever_arm_sigma = 0.10;
    vehicle_estimator::Estimator est(config);
    sim::Scenario sc(sim::figureEight(), sensors);
    const auto r = harness::run(sc, est, 20.0);
    harness::report("lever arm", r);
    const Eigen::Vector3d err = r.status.lever_arm - sensors.lever_arm;
    SPDLOG_INFO("lever arm error [{:.3f} {:.3f} {:.3f}] m, sigma [{:.3f} {:.3f} {:.3f}]", err.x(), err.y(), err.z(),
                r.status.lever_arm_sigma.x(), r.status.lever_arm_sigma.y(), r.status.lever_arm_sigma.z());
    check(err.head<2>().norm() < 0.015, "horizontal lever arm recovered to 1.5 cm while turning");
    check(r.errors.yaw < 0.5 * kDeg, "and the attitude is right");
}

void testLeverArmStraight()
{
    // Driving straight, a lever arm error along the direction of travel looks
    // exactly like a position offset: nothing observes it. The estimate must
    // hold its prior rather than drift somewhere plausible.
    sim::SensorModel sensors;
    sensors.seed = 13;
    auto config = harness::configFor(sensors);
    const Eigen::Vector3d prior = sensors.lever_arm + Eigen::Vector3d(0.05, 0.0, 0.0);
    config.lever_arm = prior;
    vehicle_estimator::Estimator est(config);
    sim::SkidpadParams p;
    p.radius = 1e5;  // straight, to any measurement
    p.beta_low = p.beta_high = 0.0;
    p.drive = 25.0;
    sim::Scenario sc(sim::skidpad(p), sensors);
    const auto r = harness::run(sc, est, 10.0);
    harness::report("lever arm straight", r);
    const double moved = (r.status.lever_arm - prior).norm();
    SPDLOG_INFO("straight: lever arm moved {:.4f} m from its prior (sigma {:.3f})", moved, r.status.lever_arm_sigma.x());
    check(moved < 0.02, "an unobservable lever arm stays near its prior");
    check(r.status.lever_arm_sigma.x() > 0.01, "and says it is still uncertain");
}

void testBoresight()
{
    // The second antenna was mounted 1.5 degrees off where the config says.
    sim::SensorModel sensors;
    sensors.seed = 14;
    auto config = harness::configFor(sensors);
    const double off = 1.5 * kDeg;
    const Eigen::Vector3d d = sensors.antenna2_lever_arm - sensors.lever_arm;
    config.antenna2_lever_arm =
        sensors.lever_arm + Eigen::AngleAxisd(off, Eigen::Vector3d::UnitZ()).toRotationMatrix() * d;
    config.boresight_sigma = 0.05;
    vehicle_estimator::Estimator est(config);
    sim::Scenario sc(sim::figureEight(), sensors);
    const auto r = harness::run(sc, est, 20.0);
    harness::report("boresight", r);
    SPDLOG_INFO("boresight estimate [{:.4f} {:.4f}] rad for a {:.4f} rad mounting error", r.status.boresight.x(),
                r.status.boresight.y(), off);
    check(std::fabs(r.status.boresight.norm() - off) < 0.1 * kDeg, "the boresight absorbs the mounting error");
    check(r.errors.yaw < 0.3 * kDeg, fmt::format("so yaw is right despite it ({:.3f} deg)", r.errors.yaw / kDeg));
}

void testTimeOffset()
{
    // Arrival-time alignment cannot see a difference in the two streams'
    // latency floors. Uncalibrated, 28 ms at racing speed is half a metre and
    // most of a degree; calibrated, it is gone. This is the test that says
    // why EstimatorConfig::imu_time_offset exists.
    sim::SensorModel sensors;
    sensors.seed = 15;
    sim::Scenario sc(sim::skidpad(), sensors);

    auto wrong = harness::configFor(sensors);
    wrong.imu_time_offset = 0.0;
    vehicle_estimator::Estimator a(wrong);
    const auto ra = harness::run(sc, a, 10.0);
    vehicle_estimator::Estimator b(harness::configFor(sensors));
    const auto rb = harness::run(sc, b, 10.0);
    SPDLOG_INFO("time offset: uncalibrated sideslip {:.3f} deg, calibrated {:.3f} deg", ra.errors.sideslip / kDeg,
                rb.errors.sideslip / kDeg);
    check(ra.errors.sideslip > 3.0 * rb.errors.sideslip, "an uncalibrated latency floor is visible");
    check(rb.errors.sideslip < 0.5 * kDeg, "and calibrating it removes it");
}

}  // namespace

int main()
{
    testBiases();
    testLeverArm();
    testLeverArmStraight();
    testBoresight();
    testTimeOffset();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("calibration: all passed");
    return 0;
}
