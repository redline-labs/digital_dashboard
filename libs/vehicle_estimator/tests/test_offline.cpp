// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offline smoother against the fixed-lag one, on the same drives.
//
// Every keyframe of the batch solution has seen all the data; every keyframe
// of the fixed-lag one only what came before it (and a few seconds after).
// So the batch must be at least as good everywhere that matters and clearly
// better where the forward pass is weakest: the start, before the biases
// are learned, and across a GNSS outage, now bridged from both sides.

#include "harness.h"

#include "vehicle_estimator/offline.h"

#include <cmath>

using harness::check;
using harness::kDeg;
namespace sim = vehicle_estimator::sim;

namespace
{

struct Rms
{
    double position = 0.0, yaw = 0.0, sideslip = 0.0;
    std::size_t n = 0, n_slip = 0;

    void add(const vehicle_estimator::VehicleState& s, const sim::Truth& t)
    {
        position += (s.p_e - t.p_e).squaredNorm();
        yaw += std::pow(harness::wrap(s.yaw - t.yaw), 2);
        ++n;
        if (s.sideslip_valid && std::hypot(t.v_body.x(), t.v_body.y()) > 3.0)
        {
            sideslip += std::pow(harness::wrap(s.sideslip - t.sideslip), 2);
            ++n_slip;
        }
    }
    Rms done() const
    {
        Rms r = *this;
        r.position = std::sqrt(position / std::max<double>(1.0, static_cast<double>(n)));
        r.yaw = std::sqrt(yaw / std::max<double>(1.0, static_cast<double>(n)));
        r.sideslip = std::sqrt(sideslip / std::max<double>(1.0, static_cast<double>(n_slip)));
        return r;
    }
};

struct Compared
{
    Rms fls, batch;
};

Compared compare(const sim::Scenario& sc, const vehicle_estimator::EstimatorConfig& config, double t0, double t1,
                 std::size_t* refused = nullptr)
{
    vehicle_estimator::Estimator est(config);
    vehicle_estimator::OfflineSmoother offline(config);
    est.setKeyframeSink([&](const vehicle_estimator::KeyframeRecord& r) { offline.add(r); });
    const auto run = harness::run(sc, est, 0.0);
    const auto result = offline.solve();
    if (refused) *refused = result.refused;

    Compared c;
    for (std::size_t k = 0; k < run.states.size(); ++k)
    {
        const double t = sc.toScenarioTime(run.states[k].gps_time);
        if (t >= t0 && t < t1) c.fls.add(run.states[k], run.truths[k]);
    }
    for (const auto& s : result.states)
    {
        const double t = sc.toScenarioTime(s.gps_time);
        if (t >= t0 && t < t1) c.batch.add(s, sc.truth(t));
    }
    c.fls = c.fls.done();
    c.batch = c.batch.done();
    return c;
}

void log(const std::string& label, const Compared& c)
{
    SPDLOG_INFO("{}: fixed-lag rms position {:.3f} m, yaw {:.3f} deg, sideslip {:.3f} deg; batch {:.3f} m, {:.3f} deg, "
                "{:.3f} deg ({} keyframes)",
                label, c.fls.position, c.fls.yaw / kDeg, c.fls.sideslip / kDeg, c.batch.position, c.batch.yaw / kDeg,
                c.batch.sideslip / kDeg, c.batch.n);
}

void testWholeDrive()
{
    sim::SensorModel sensors;
    sensors.seed = 41;
    sim::Scenario sc(sim::figureEight(), sensors);
    const auto config = harness::configFor(sensors);
    const auto all = compare(sc, config, 5.0, 50.0);
    log("whole drive", all);
    check(all.batch.n > 400, "the batch estimated every keyframe");
    check(all.batch.position <= all.fls.position * 1.05, "batch position no worse");
    check(all.batch.yaw <= all.fls.yaw * 1.05, "batch yaw no worse");
    check(all.batch.sideslip <= all.fls.sideslip * 1.05, "batch sideslip no worse");

    // The first seconds after the start, where the fixed-lag smoother is
    // still learning the biases and the batch already knows them.
    const auto early = compare(sc, config, 1.0, 8.0);
    log("first seconds", early);
    check(early.batch.yaw < early.fls.yaw, "batch better at the start");
}

void testOutage()
{
    sim::SensorModel sensors;
    sensors.seed = 42;
    sensors.outages = {{25.0, 30.0}};
    sim::Scenario sc(sim::skidpad(), sensors);
    const auto config = harness::configFor(sensors);
    // The keyframes just after the outage: the fixed-lag smoother arrives
    // there having dead-reckoned five seconds; the batch has the fixes on
    // both sides.
    const auto after = compare(sc, config, 30.0, 31.0);
    log("after the outage", after);
    check(after.batch.position < after.fls.position, "batch better where the outage ends");
}

void testRestart()
{
    // A restart splits the drive; the batch must still take every record
    // (the restart's re-declared statics included) and solve both halves.
    sim::SensorModel sensors;
    sensors.seed = 43;
    for (std::size_t k = 2000; k < 2050; ++k) sensors.dropped_imu.push_back(k);
    sim::Scenario sc(sim::skidpad(), sensors);
    std::size_t refused = 1;
    const auto c = compare(sc, harness::configFor(sensors), 10.0, 45.0, &refused);
    log("restart", c);
    check(refused == 0, "no record refused across a restart");
    check(c.batch.position < 0.10 && c.batch.yaw < 0.5 * kDeg, "both halves solved");
}

void testCalibrationHistory()
{
    // The lever arm configured 5 cm out. The forward pass starts at the prior
    // and learns it as the car turns; the batch has the whole drive, and the
    // walk carries what was learned late back to the first segment. With the
    // segments not joined, the first would sit at its prior in both.
    sim::SensorModel sensors;
    sensors.seed = 44;
    sim::Scenario sc(sim::figureEight(), sensors);
    auto config = harness::configFor(sensors);
    config.lever_arm += Eigen::Vector3d(0.05, 0.0, 0.0);
    config.lever_arm_sigma = 0.1;

    vehicle_estimator::Estimator est(config);
    vehicle_estimator::OfflineSmoother offline(config);
    est.setKeyframeSink([&](const vehicle_estimator::KeyframeRecord& r) { offline.add(r); });
    harness::run(sc, est, 0.0);
    const auto result = offline.solve();

    const auto& h = result.calibration;
    const double drive = sc.duration();
    SPDLOG_INFO("calibration history: {} segments over {:.0f} s; first lever arm error {:.4f} m, last {:.4f} m",
                h.size(), drive, h.empty() ? 0.0 : (h.front().set.lever_arm - sensors.lever_arm).norm(),
                h.empty() ? 0.0 : (h.back().set.lever_arm - sensors.lever_arm).norm());
    check(h.size() + 5 >= static_cast<std::size_t>(drive / config.calibration_segment) - 5,
          "about one segment per second of drive");
    bool increasing = true;
    for (std::size_t i = 1; i < h.size(); ++i) increasing = increasing && h[i].t > h[i - 1].t;
    check(increasing, "segments in time order");
    check(!h.empty() && (h.back().set.lever_arm - sensors.lever_arm).norm() < 0.01, "the lever arm is learned");
    check(!h.empty() && (h.front().set.lever_arm - sensors.lever_arm).norm() < 0.01,
          "and the batch carries it back to the start of the drive");
    if (!h.empty())
    {
        const Eigen::Vector3d sigma = h.back().set.leverArmCov().diagonal().cwiseSqrt();
        const Eigen::Vector3d err = h.back().set.lever_arm - sensors.lever_arm;
        SPDLOG_INFO("lever arm sigma [{:.4f} {:.4f} {:.4f}] m, error [{:.4f} {:.4f} {:.4f}] m", sigma.x(), sigma.y(),
                    sigma.z(), err.x(), err.y(), err.z());
        check(sigma.x() < 0.01 && std::fabs(err.x()) < 3.0 * sigma.x(),
              "with a covariance that says so, along the axis that was wrong");
    }
}

}  // namespace

int main()
{
    testWholeDrive();
    testOutage();
    testRestart();
    testCalibrationHistory();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("offline: all passed");
    return 0;
}
