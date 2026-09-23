// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node end to end, without a bus: a simulated drift, encoded as the capnp
// messages the two bridges would publish, through decode, record pairing and
// the estimator. What this catches that the library's own tests cannot is a
// field read from the wrong place, a unit conversion missed, a sigma taken as
// east where it was north -- everything between the wire and the estimator.

#include "pipeline.h"
#include "sim_bus.h"

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

constexpr double kDeg = std::numbers::pi / 180.0;
namespace sim = vehicle_estimator::sim;

double wrap(double a)
{
    return std::remainder(a, 2.0 * std::numbers::pi);
}

}  // namespace

int main()
{
    sim::SensorModel sensors;
    sensors.seed = 61;
    sensors.reference_point = Eigen::Vector3d(-0.8, 0.0, -0.2);
    sim::Scenario sc(sim::skidpad(), sensors);

    state_estimator::NodeConfig config;
    config.leverArmM = sensors.lever_arm;
    config.antenna2LeverArmM = sensors.antenna2_lever_arm;
    config.referencePointM = sensors.reference_point;
    config.imuTimeOffsetS = sensors.gnss_latency - sensors.imu_latency;
    state_estimator::Pipeline pipeline(config);

    const auto bus = state_estimator::toBus(sc);
    std::size_t used = 0, states = 0;
    double worst_yaw = 0.0, worst_slip = 0.0, worst_pos = 0.0;
    for (const auto& m : bus)
    {
        if (pipeline.onMessage(m.schema, m.payload, m.arrival) == state_estimator::Fed::used) ++used;
        for (const auto& s : pipeline.advance(m.arrival))
        {
            ++states;
            const double t = sc.toScenarioTime(s.gps_time);
            if (t < 12.0) continue;
            const auto truth = sc.truth(t);
            worst_yaw = std::max(worst_yaw, std::fabs(wrap(s.yaw - truth.yaw)));
            worst_pos = std::max(worst_pos, (s.p_e - truth.p_e).norm());
            if (s.sideslip_valid) worst_slip = std::max(worst_slip, std::fabs(wrap(s.sideslip - truth.sideslip)));
        }
    }
    const auto& st = pipeline.estimator().status();
    SPDLOG_INFO("pipeline: {} messages ({} used, {} malformed), {} epochs assembled ({} undated), {} keyframes, {} "
                "states; worst yaw {:.3f} deg, sideslip {:.3f} deg, position {:.3f} m",
                bus.size(), used, pipeline.malformed(), pipeline.gnss().stats().epochs, pipeline.gnss().stats().noTime,
                st.keyframes, states, worst_yaw / kDeg, worst_slip / kDeg, worst_pos);

    check(used == bus.size(), "every message decoded");
    check(pipeline.malformed() == 0, "none malformed");
    check(pipeline.gnss().stats().noTime == 0, "every epoch dated");
    check(pipeline.imu().orphans() == 0, "every IMU half paired");
    check(st.keyframes > 400, "keyframes from the assembled epochs");
    check(states > 4000, "a state per IMU sample");
    check(worst_yaw < 0.5 * kDeg, "yaw through the wire");
    check(worst_slip < 0.5 * kDeg, "sideslip through the wire");
    check(worst_pos < 0.10, "position through the wire");
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("pipeline: all passed");
    return 0;
}
