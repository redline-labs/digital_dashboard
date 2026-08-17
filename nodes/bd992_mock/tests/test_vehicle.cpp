// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vehicle model, against limits it must not exceed.
//
// The test worth having here is the corner one. Without the backward relaxation
// in buildSpeedProfile the vehicle arrives at a hairpin at cruise speed and
// drops to the corner speed in a single tick -- which is a deceleration of a few
// hundred m/s^2 and a lateral acceleration to match. Nothing errors; the
// position stream just contains a discontinuity that reads downstream as a GNSS
// glitch. Deleting that pass must fail this file.

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "path.h"
#include "vehicle.h"

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

void checkNear(double actual, double expected, double tolerance, const std::string& what)
{
    if (!(std::fabs(actual - expected) <= tolerance))
    {
        SPDLOG_ERROR("FAIL: {} (got {}, expected {} +/- {})", what, actual, expected, tolerance);
        ++failures;
    }
}

using bd992_mock::Path;
using bd992_mock::Vehicle;
using bd992_mock::VehicleConfig;

constexpr double kIrvineLat = 33.6866;
constexpr double kIrvineLon = -117.8558;
constexpr double kMetresPerDegreeLat = 111132.0;
constexpr double kDegPerRad = 180.0 / std::numbers::pi;

VehicleConfig testConfig()
{
    VehicleConfig config;
    config.cruiseSpeedMps = 30.0;
    config.accelMps2 = 2.0;
    config.brakeMps2 = 4.0;
    config.lateralAccelMps2 = 3.0;
    config.headingLookaheadM = 5.0;
    return config;
}

// A northward straight of `lengthM`, sampled every `stepM`.
Path straight(double lengthM, double stepM)
{
    Path path;
    const auto points = static_cast<std::size_t>(lengthM / stepM) + 1;
    for (std::size_t i = 0; i < points; ++i)
    {
        const double north = static_cast<double>(i) * stepM;
        path.lat.push_back(road_graph::fromDegrees(kIrvineLat + (north / kMetresPerDegreeLat)));
        path.lon.push_back(road_graph::fromDegrees(kIrvineLon));
    }
    bd992_mock::computeDistances(path);
    path.speedCapMps.assign(path.size(), std::numeric_limits<double>::infinity());
    return path;
}

// A closed circle of `radiusM`.
Path circle(double radiusM, std::size_t points)
{
    const double cosLat = std::cos(kIrvineLat / kDegPerRad);

    Path path;
    for (std::size_t i = 0; i < points; ++i)
    {
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(points);
        const double northM = radiusM * std::sin(angle);
        const double eastM = radiusM * (1.0 - std::cos(angle));

        path.lat.push_back(road_graph::fromDegrees(kIrvineLat + (northM / kMetresPerDegreeLat)));
        path.lon.push_back(
            road_graph::fromDegrees(kIrvineLon + (eastM / (kMetresPerDegreeLat * cosLat))));
    }
    bd992_mock::computeDistances(path);
    path.speedCapMps.assign(path.size(), std::numeric_limits<double>::infinity());
    path.closed = true;
    return path;
}

// A long straight running into a tight constant-radius corner.
Path straightIntoCorner(double straightM, double radiusM)
{
    const double cosLat = std::cos(kIrvineLat / kDegPerRad);

    Path path;

    // The straight, heading north.
    constexpr double kStep = 10.0;
    const auto straightPoints = static_cast<std::size_t>(straightM / kStep) + 1;
    for (std::size_t i = 0; i < straightPoints; ++i)
    {
        const double north = static_cast<double>(i) * kStep;
        path.lat.push_back(road_graph::fromDegrees(kIrvineLat + (north / kMetresPerDegreeLat)));
        path.lon.push_back(road_graph::fromDegrees(kIrvineLon));
    }

    // Then a 180-degree turn of the given radius, densely sampled.
    constexpr std::size_t kCornerPoints = 90;
    for (std::size_t i = 1; i <= kCornerPoints; ++i)
    {
        const double angle =
            std::numbers::pi * static_cast<double>(i) / static_cast<double>(kCornerPoints);
        const double northM = straightM + (radiusM * std::sin(angle));
        const double eastM = radiusM * (1.0 - std::cos(angle));

        path.lat.push_back(road_graph::fromDegrees(kIrvineLat + (northM / kMetresPerDegreeLat)));
        path.lon.push_back(
            road_graph::fromDegrees(kIrvineLon + (eastM / (kMetresPerDegreeLat * cosLat))));
    }

    bd992_mock::computeDistances(path);
    path.speedCapMps.assign(path.size(), std::numeric_limits<double>::infinity());
    return path;
}

// ============================================================================

void test_a_straight_reaches_cruise_and_stops_at_the_end()
{
    const Path path = straight(2000.0, 20.0);
    const VehicleConfig config = testConfig();
    Vehicle vehicle(path, config, false);

    constexpr double dt = 0.1;
    double fastest = 0.0;
    for (int tick = 0; tick < 3000; ++tick)
    {
        vehicle.step(dt);
        fastest = std::max(fastest, vehicle.state().speedMps);
    }

    checkNear(fastest, config.cruiseSpeedMps, 0.5, "a long straight reaches the cruise speed");
    checkNear(vehicle.state().speedMps, 0.0, 0.1, "and comes to rest at the end");
    checkNear(vehicle.state().alongM, path.lengthM(), 1.0, "having driven the whole thing");
    check(!vehicle.state().moving, "and reports itself stopped");
    check(vehicle.state().lap == 0, "an open path driven once is not a lap");
}

void test_longitudinal_acceleration_stays_within_limits()
{
    const Path path = straightIntoCorner(600.0, 25.0);
    const VehicleConfig config = testConfig();
    Vehicle vehicle(path, config, false);

    constexpr double dt = 0.1;
    double previous = vehicle.state().speedMps;
    double hardestAccel = 0.0;
    double hardestBrake = 0.0;

    for (int tick = 0; tick < 2000; ++tick)
    {
        vehicle.step(dt);
        const double now = vehicle.state().speedMps;
        const double rate = (now - previous) / dt;
        hardestAccel = std::max(hardestAccel, rate);
        hardestBrake = std::max(hardestBrake, -rate);
        previous = now;
    }

    // A small tolerance for the discretisation: the integrator clamps to the
    // profile, which can land between two ticks.
    check(hardestAccel <= config.accelMps2 + 0.01,
          "acceleration never exceeds the configured limit (peak " +
              std::to_string(hardestAccel) + ")");
    check(hardestBrake <= config.brakeMps2 + 0.01,
          "braking never exceeds it either (peak " + std::to_string(hardestBrake) + ")");
}

// THE ONE THAT PINS THE BACKWARD PASS.
void test_a_corner_is_never_taken_faster_than_it_can_be_held()
{
    constexpr double kRadius = 25.0;
    const Path path = straightIntoCorner(600.0, kRadius);
    const VehicleConfig config = testConfig();
    Vehicle vehicle(path, config, false);

    // What the corner physically allows.
    const double allowed = std::sqrt(config.lateralAccelMps2 * kRadius);

    constexpr double dt = 0.05;
    double worstLateral = 0.0;

    for (int tick = 0; tick < 4000; ++tick)
    {
        vehicle.step(dt);

        // Where the vehicle is now, and how tight the road is there.
        const double s = vehicle.state().alongM;
        const double v = vehicle.state().speedMps;

        // Find the vertex nearest the current distance and use its radius.
        std::size_t nearest = 0;
        double best = 1e18;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            const double gap = std::fabs(path.distanceM[i] - s);
            if (gap < best)
            {
                best = gap;
                nearest = i;
            }
        }

        const double radius = bd992_mock::curvatureRadiusM(path, nearest, config.curvatureBaselineM);
        if (std::isfinite(radius) && radius > 0.0)
        {
            worstLateral = std::max(worstLateral, (v * v) / radius);
        }
    }

    // 10% of headroom for the discretisation and for the fact that the sampled
    // radius wobbles slightly around the true one.
    check(worstLateral <= config.lateralAccelMps2 * 1.1,
          "lateral acceleration never exceeds the limit -- braking starts before the corner "
          "(peak " +
              std::to_string(worstLateral) + " against " +
              std::to_string(config.lateralAccelMps2) + ")");

    // And the corner really was reached, so the assertion above was not vacuous.
    check(vehicle.state().alongM > 600.0, "the vehicle actually got into the corner");
    check(allowed < config.cruiseSpeedMps,
          "the corner is genuinely slower than cruise, so there was something to brake for");
}

void test_steady_state_speed_on_a_circle()
{
    constexpr double kRadius = 60.0;
    const Path path = circle(kRadius, 180);
    const VehicleConfig config = testConfig();
    Vehicle vehicle(path, config, false);

    constexpr double dt = 0.1;
    // Long enough to settle.
    for (int tick = 0; tick < 2000; ++tick)
    {
        vehicle.step(dt);
    }

    const double expected = std::sqrt(config.lateralAccelMps2 * kRadius);
    checkNear(vehicle.state().speedMps, expected, expected * 0.05,
              "a constant-radius circle settles at sqrt(a_lat * R)");
}

void test_a_closed_path_laps_and_an_open_one_does_not()
{
    const Path loop = circle(60.0, 180);
    Vehicle lapping(loop, testConfig(), false);

    constexpr double dt = 0.1;
    for (int tick = 0; tick < 4000; ++tick)
    {
        lapping.step(dt);
    }

    check(lapping.state().lap >= 1, "a circuit completes laps");
    check(lapping.state().alongM <= loop.lengthM(),
          "and its distance wraps rather than growing forever");

    const Path open = straight(500.0, 20.0);
    Vehicle once(open, testConfig(), false);
    for (int tick = 0; tick < 4000; ++tick)
    {
        once.step(dt);
    }
    check(once.state().lap == 0, "an open path without --loop never laps");
}

void test_an_open_path_with_loop_restarts()
{
    const Path path = straight(300.0, 20.0);
    Vehicle vehicle(path, testConfig(), true);

    constexpr double dt = 0.1;
    for (int tick = 0; tick < 1000; ++tick)
    {
        vehicle.step(dt);
    }

    check(vehicle.state().lap >= 1, "--loop restarts an open path at its beginning");
    check(vehicle.state().alongM < path.lengthM(), "and it is somewhere along it again");
}

void test_a_posted_limit_caps_the_speed()
{
    Path path = straight(2000.0, 20.0);
    // 40 km/h over the whole thing.
    const double limit = 40.0 * 1000.0 / 3600.0;
    path.speedCapMps.assign(path.size(), limit);

    const VehicleConfig config = testConfig();
    Vehicle vehicle(path, config, false);

    constexpr double dt = 0.1;
    double fastest = 0.0;
    for (int tick = 0; tick < 2000; ++tick)
    {
        vehicle.step(dt);
        fastest = std::max(fastest, vehicle.state().speedMps);
    }

    checkNear(fastest, limit, 0.2, "a posted limit below cruise speed is what gets driven");
}

void test_the_speed_profile_never_exceeds_the_caps()
{
    const Path path = straightIntoCorner(400.0, 30.0);
    const VehicleConfig config = testConfig();
    const std::vector<double> profile = bd992_mock::buildSpeedProfile(path, config);

    check(profile.size() == path.size(), "the profile has one entry per vertex");

    for (std::size_t i = 0; i < path.size(); ++i)
    {
        check(profile[i] <= config.cruiseSpeedMps + 1e-9, "no vertex exceeds the cruise speed");

        const double radius = bd992_mock::curvatureRadiusM(path, i, config.curvatureBaselineM);
        if (std::isfinite(radius) && radius > 0.0)
        {
            const double allowed = std::sqrt(config.lateralAccelMps2 * radius);
            check(profile[i] <= allowed + 1e-6, "no vertex exceeds what its own curvature allows");
        }
    }

    checkNear(profile.back(), 0.0, 1e-9, "an open path's profile ends at rest");
}

} // namespace

int main()
{
    // No level squelch here: nothing in this file logs an expected error, so a
    // failed check must be able to say which one it was.
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_straight_reaches_cruise_and_stops_at_the_end();
    test_longitudinal_acceleration_stays_within_limits();
    test_a_corner_is_never_taken_faster_than_it_can_be_held();
    test_steady_state_speed_on_a_circle();
    test_a_closed_path_laps_and_an_open_one_does_not();
    test_an_open_path_with_loop_restarts();
    test_a_posted_limit_caps_the_speed();
    test_the_speed_profile_never_exceeds_the_caps();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all bd992_mock vehicle checks passed");
    return 0;
}
