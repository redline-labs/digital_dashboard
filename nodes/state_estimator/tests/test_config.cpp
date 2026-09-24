// SPDX-License-Identifier: GPL-3.0-or-later
//
// The config surface: the shipped file parses, the geometry reaches the
// estimator in the units it expects, and every value that would start the
// node and then estimate wrongly is refused at startup instead.

#include "node_config.h"

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

using state_estimator::NodeConfig;

bool parses(const std::string& yaml)
{
    NodeConfig c;
    return state_estimator::parse_node_config(yaml, c);
}

void testShipped()
{
    NodeConfig c;
    check(state_estimator::load_node_config(REDLINE_SOURCE_DIR "/configs/state_estimator/state_estimator.yaml", c),
          "the shipped config parses");
}

void testUnits()
{
    NodeConfig c;
    check(state_estimator::parse_node_config(R"(
vehicle:
  imu_to_body_rpy_deg: [180, 0, 90]
  reference_point_m: [-0.8, 0, -0.2]
  lever_arm_m: [0.3, 0, 1.2]
  antenna2_lever_arm_m: [-1.2, 0, 1.2]
  boresight_sigma_deg: 2
imu:
  gyro_noise_density_dps: 0.01
  dv_frame: start
smoother:
  time_budget_ms: 25
)",
                                             c),
          "a full config parses");
    const auto e = state_estimator::estimatorConfig(c);
    check(std::fabs(e.boresight_sigma - 2.0 * std::numbers::pi / 180.0) < 1e-15, "degrees become radians");
    check(std::fabs(e.imu_noise.gyro_noise_density - 0.01 * std::numbers::pi / 180.0) < 1e-15, "gyro noise too");
    check(e.dv_frame == imu_preint::DvFrame::start, "dv frame");
    check(e.lm.time_budget && std::fabs(e.lm.time_budget->count() - 0.025) < 1e-12, "milliseconds become seconds");
    // roll 180 then yaw 90: IMU x -> body y, IMU z -> body -z.
    const Eigen::Vector3d x = e.R_b_i * Eigen::Vector3d::UnitX(), z = e.R_b_i * Eigen::Vector3d::UnitZ();
    check((x - Eigen::Vector3d::UnitY()).norm() < 1e-12 && (z + Eigen::Vector3d::UnitZ()).norm() < 1e-12,
          "mounting angles compose yaw-pitch-roll");
}

void testCalibration()
{
    NodeConfig c;
    check(state_estimator::parse_node_config(R"(
vehicle:
  imu_to_body_sigma_deg: [1, 1, 3]
calibration:
  database: "${REDLINE_DATA_DIR}/cal.sqlite"
  min_write_interval_s: 600
  load_inflation: 9
  segment_s: 0.5
  mounting_walk_deg_per_sqrt_h: 0.6
  lever_arm_walk_mm_per_sqrt_h: 6
)",
                                             c),
          "a calibration section parses");
    const auto e = state_estimator::estimatorConfig(c);
    constexpr double kDeg = std::numbers::pi / 180.0;
    check(std::fabs(e.mounting_sigma.z() - 3.0 * kDeg) < 1e-12, "mounting sigma arrives in radians");
    check(std::fabs(e.mounting_walk - 0.01 * kDeg) < 1e-15, "0.6 deg/sqrt(h) is 0.01 deg/sqrt(s)");
    check(std::fabs(e.lever_arm_walk - 1e-4) < 1e-15, "6 mm/sqrt(h) is 0.1 mm/sqrt(s)");
    check(e.calibration_segment == 0.5, "segment length carried");
    check(c.calibration.minWriteIntervalS == 600.0 && c.calibration.loadInflation == 9.0, "policy carried");

    check(!parses("calibration:\n  load_inflation: 0.5\n"), "inflation below 1 would make a stored value MORE sure");
    check(!parses("calibration:\n  min_write_interval_s: -1\n"), "a negative interval is refused");
    check(!parses("smoother:\n  lag_s: 1.0\ncalibration:\n  segment_s: 0.6\n"),
          "a segment longer than half the lag is refused");
    check(!parses("calibration:\n  tighten_ratio: 1.0\n"), "a tighten ratio of 1 would write every tick");
    check(!parses("calibration:\n  mounting_walk_deg_per_sqrt_h: 0\n"), "a zero walk is refused");
    check(!parses("calibration:\n  moved_sigma: .nan\n"), "a non-finite sigma is refused");
    check(!parses("vehicle:\n  imu_to_body_sigma_deg: [1, 0, 1]\n"), "a zero mounting sigma is refused");
    check(!parses("calibration:\n  enabled: sometimes\n"), "enabled must be a boolean");
    check(!parses("calibration:\n  database: ''\n"), "an empty database path is refused while enabled");
    check(parses("calibration:\n  enabled: false\n  database: ''\n"), "but fine when disabled");
}

void testRefusals()
{
    check(!parses("not: [a mapping"), "malformed YAML");
    check(!parses("- a\n- list\n"), "a list, not a mapping");
    check(!parses("inputs:\n  imu_prefix: 'nodes/mti*'\n"), "a wildcard in a prefix");
    check(!parses("outputs:\n  state_key: a/b\n  status_key: a/b\n"), "state and status on one key");
    check(!parses("outputs:\n  state_decimation: 0\n"), "zero decimation");
    check(!parses("vehicle:\n  lever_arm_m: [1, 2]\n"), "a two-element lever arm");
    check(!parses("vehicle:\n  lever_arm_m: [1, two, 3]\n"), "a lever arm that is not numbers");
    check(!parses("vehicle:\n  lever_arm_m: [.nan, 0, 0]\n"), "a NaN lever arm");
    check(!parses("vehicle:\n  lever_arm_m: [0, 0, 1]\n  antenna2_lever_arm_m: [0, 0.01, 1]\n"),
          "antennas a centimetre apart");
    check(!parses("vehicle:\n  lever_arm_sigma_m: -1\n"), "a negative sigma");
    check(!parses("imu:\n  dv_frame: middle\n"), "an unknown dv frame");
    check(!parses("imu:\n  time_offset_s: 3\n"), "a three-second 'latency'");
    check(!parses("imu:\n  gyro_noise_density_dps: 0\n"), "a noiseless gyro");
    check(!parses("smoother:\n  lag_s: 0\n"), "no lag");
    check(!parses("smoother:\n  max_iterations: 0\n"), "no iterations");
    check(!parses("gnss:\n  burst_ms: -5\n"), "a negative burst window");
    check(!parses("vehicle: 7\n"), "a section that is not a mapping");
    check(parses("{}"), "an empty document takes every default");
}

}  // namespace

int main()
{
    testShipped();
    testUnits();
    testRefusals();
    testCalibration();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("config: all passed");
    return 0;
}
