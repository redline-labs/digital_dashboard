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
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("config: all passed");
    return 0;
}
