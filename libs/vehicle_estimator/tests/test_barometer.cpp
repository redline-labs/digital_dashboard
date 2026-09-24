// SPDX-License-Identifier: GPL-3.0-or-later
//
// The barometer: what it learns about itself while GNSS is good, and what it
// is worth when GNSS is not. An IMU's vertical channel is unstable -- height
// error grows with the square of time from any accelerometer bias -- so an
// outage on a climbing road is where it has to earn its place. With good RTX
// the GNSS height is better than a barometer's and the smoother should know it.

#include "harness.h"

#include "vehicle_estimator/atmosphere.h"

#include "geodesy/geodetic.h"

using harness::check;
namespace sim = vehicle_estimator::sim;

namespace
{

// Up and down a hill, faster and slower: the height changes the barometer
// tracks, and the speed changes that separate the airflow from the offset.
std::unique_ptr<sim::VehicleMotion> hills(int laps = 4)
{
    std::vector<sim::Phase> p;
    p.push_back({5.0, 0.0});
    p.push_back({6.0, 18.0});
    for (int lap = 0; lap < laps; ++lap)
    {
        p.push_back({12.0, 26.0, 0.0, 0.0, 0.0, 0.0, 0.0, 25.0});   // up, fast
        p.push_back({10.0, 14.0, std::numbers::pi, 0.2});             // round, slow
        p.push_back({12.0, 22.0, 0.0, 0.0, 0.0, 0.0, 0.0, -25.0});  // down
        p.push_back({10.0, 14.0, std::numbers::pi, 0.2});
    }
    return sim::scripted(std::move(p));
}

double heightOf(const Eigen::Vector3d& p_e)
{
    return geodesy::ecefToLlh(csym::Vector3<double>{p_e.x(), p_e.y(), p_e.z()}).h;
}

// Worst |vertical error| over keyframes in [t0, t1).
double worstVertical(const harness::Run& r, const sim::Scenario& sc, double t0, double t1)
{
    double worst = 0.0;
    for (std::size_t k = 0; k < r.states.size(); ++k)
    {
        const double t = sc.toScenarioTime(r.states[k].gps_time);
        if (t < t0 || t >= t1) continue;
        worst = std::max(worst, std::fabs(r.states[k].h - heightOf(r.truths[k].p_e)));
    }
    return worst;
}

void testLearnedWithGnss()
{
    sim::SensorModel sensors;
    sensors.seed = 61;
    sim::Scenario sc(hills(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto r = harness::run(sc, est, 10.0);
    harness::report("barometer learns", r);
    const auto& cal = *est.calibration();
    SPDLOG_INFO("barometer: offset {:.2f} m (truth {:.2f}), airflow {:.3f} (truth {:.3f}); {} factors, {:.0f} s moving",
                cal.baro_offset, sensors.baro_offset, cal.baro_airflow, sensors.baro_airflow, r.status.baro_factors,
                r.status.baro_moving_s);
    check(r.status.baro_factors > 1500, "a barometric height on the keyframes");
    check(std::fabs(cal.baro_offset - sensors.baro_offset) < 1.0, "the offset learned to a metre");
    check(std::fabs(cal.baro_airflow - sensors.baro_airflow) < 0.1, "the airflow learned from the speed changes");
    check(std::sqrt(cal.barometerCov()(1, 1)) < 0.1, "and says so");
}

double outageVertical(bool barometer)
{
    // A minute without GNSS, on the way up and down the hills.
    sim::SensorModel sensors;
    sensors.seed = 62;
    sensors.outages = {{80.0, 140.0}};
    sim::Scenario sc(hills(), sensors);
    auto config = harness::configFor(sensors);
    config.use_barometer = barometer;
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    const double worst = worstVertical(r, sc, 80.0, 140.0);
    SPDLOG_INFO("60 s outage, barometer {}: worst vertical error {:.3f} m", barometer ? "on" : "off", worst);
    return worst;
}

void testOutage()
{
    const double with = outageVertical(true);
    const double without = outageVertical(false);
    // Measured 2026-09-24: 0.26 m against 0.57 m. The IMU alone does better
    // than its reputation because the accelerometer bias was learned before
    // the outage; the barometer's margin grows with the outage. (The IMU's
    // 0.78 m of 2026-09-23 was a solver stopping short of its minimum.)
    check(with < 0.5, "through a minute's outage the barometer holds height to half a metre");
    check(with * 2.0 < without, "over twice as well as the IMU alone");
}

double autonomousVertical(bool barometer)
{
    // RTX lost at 60 s: autonomous, with its sigmas and noise twenty times larger.
    sim::SensorModel sensors;
    sensors.seed = 63;
    sensors.fix_changes = {{60.0, vehicle_estimator::FixQuality::autonomous, 20.0}};
    sim::Scenario sc(hills(), sensors);
    auto config = harness::configFor(sensors);
    config.use_barometer = barometer;
    vehicle_estimator::Estimator est(config);
    const auto r = harness::run(sc, est, 10.0);
    const double worst = worstVertical(r, sc, 80.0, sc.duration());
    SPDLOG_INFO("autonomous fix, barometer {}: worst vertical error {:.3f} m", barometer ? "on" : "off", worst);
    return worst;
}

void testAutonomous()
{
    const double with = autonomousVertical(true);
    const double without = autonomousVertical(false);
    // Measured 2026-09-23: 0.74 m against 1.06 m. The simulator's autonomous
    // error is white per epoch; a real one wanders over metres for minutes,
    // which is where a barometer earns more than this shows.
    check(with * 1.3 < without, "a degraded fix's height is held by the barometer");
}

void testWeather()
{
    // The weather moving 70 m of pressure altitude an hour: the offset walks
    // and the estimate follows it.
    sim::SensorModel sensors;
    sensors.seed = 64;
    sensors.baro_offset_rate = 0.02;
    sim::Scenario sc(hills(), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    harness::run(sc, est, 10.0);
    const double truth = sensors.baro_offset + sensors.baro_offset_rate * sc.duration();
    SPDLOG_INFO("weather: offset {:.2f} m at the end, truth {:.2f}", est.calibration()->baro_offset, truth);
    check(std::fabs(est.calibration()->baro_offset - truth) < 1.0, "the offset follows the weather");
}

}  // namespace

int main()
{
    testLearnedWithGnss();
    testOutage();
    testAutonomous();
    testWeather();
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("barometer: all passed");
    return 0;
}
