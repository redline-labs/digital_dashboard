// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keyframes on the IMU's clock. With GNSS they are the GNSS epochs, as
// before; without it the estimator makes its own every 0.1 s, so an outage
// has keyframes for the IMU chain, for any other measurement, and for the
// covariance to grow on -- instead of one keyframe the output is carried
// forward from, unchanged in its sigmas, with the IMU buffer growing.

#include "harness.h"

using harness::check;
namespace sim = vehicle_estimator::sim;

namespace
{

struct Outage
{
    harness::Run run;
    std::size_t max_buffered = 0;
    double sigma_at_start = 0.0, sigma_at_end = 0.0;  // latest() horizontal position sigma
    double worst_spacing = 0.0;                       // between keyframes inside the outage
};

Outage drive(sim::Scenario& sc, vehicle_estimator::Estimator& est, double t0, double t1)
{
    Outage o;
    double last_kf = -1.0;
    std::uint64_t seen = 0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        o.max_buffered = std::max(o.max_buffered, est.status().imu_buffered);
        if (const auto s = est.latest())
        {
            const double t = sc.toScenarioTime(s->gps_time);
            const double sigma = s->sigma_position_ned.head<2>().norm();
            if (t >= t0 && o.sigma_at_start == 0.0) o.sigma_at_start = sigma;
            if (t < t1) o.sigma_at_end = sigma;
        }
        if (est.status().keyframes != seen)
        {
            seen = est.status().keyframes;
            const double t = sc.toScenarioTime(est.keyframeState()->gps_time);
            if (last_kf >= 0.0 && t > t0 + 0.6 && t < t1) o.worst_spacing = std::max(o.worst_spacing, t - last_kf);
            last_kf = t;
        }
    }
    return o;
}

void testOutage()
{
    sim::SensorModel sensors;
    sensors.seed = 51;
    sensors.outages = {{20.0, 30.0}};
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const Outage o = drive(sc, est, 20.0, 30.0);
    const auto& st = est.status();
    SPDLOG_INFO("outage: {} inertial keyframes, worst spacing {:.3f} s, IMU buffer at most {}, sigma {:.3f} -> {:.3f} m",
                st.inertial_keyframes, o.worst_spacing, o.max_buffered, o.sigma_at_start, o.sigma_at_end);
    check(st.inertial_keyframes >= 90 && st.inertial_keyframes <= 100, "a keyframe every 0.1 s through a 10 s outage");
    check(o.worst_spacing < 0.1 + 1e-6, "and no longer gap than that inside it");
    check(o.max_buffered < 100, "the IMU buffer stays under a second, outage or not");
    check(o.sigma_at_end > 5.0 * o.sigma_at_start, "the reported sigma grows through the outage");
}

void testNoneBesideGnss()
{
    // Out-of-order and late epochs: GNSS is there, just untidy. Every
    // keyframe must still be an epoch -- an inertial one pre-empting a late
    // epoch would cost that epoch.
    sim::SensorModel sensors;
    sensors.seed = 52;
    sensors.gnss_in_order = false;
    sensors.gnss_jitter = 0.05;
    sim::Scenario sc(sim::skidpad(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    SPDLOG_INFO("untidy GNSS: {} inertial keyframes, {} late, {} keyframes", r.status.inertial_keyframes,
                r.status.gnss_late, r.status.keyframes);
    check(r.status.inertial_keyframes == 0, "no inertial keyframe while GNSS is only late");
    check(r.errors.position < 0.1, "and accuracy as before");
}

void testParkedInAnOutage()
{
    // Parked, and GNSS gone for 40 s. Zero velocity holds the IMU still; the
    // estimate must not drift off at the rate an accelerometer bias allows.
    sim::SensorModel sensors;
    sensors.seed = 53;
    sensors.outages = {{10.0, 50.0}};
    sim::Scenario sc(sim::parked(55.0), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    double worst_speed = 0.0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        est.process();
        if (const auto s = est.keyframeState(); s && sc.toScenarioTime(s->gps_time) > 12.0 &&
                                                sc.toScenarioTime(s->gps_time) < 50.0)
            worst_speed = std::max(worst_speed, s->v_ned.norm());
    }
    SPDLOG_INFO("parked outage: worst speed {:.4f} m/s, {} zero-velocity updates", worst_speed,
                est.status().zero_velocity_updates);
    check(est.status().zero_velocity_updates > 300, "zero velocity through the outage");
    check(worst_speed < 0.02, "the parked car stays parked");
}

}  // namespace

int main()
{
    testOutage();
    testNoneBesideGnss();
    testParkedInAnOutage();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("keyframes: all passed");
    return 0;
}
