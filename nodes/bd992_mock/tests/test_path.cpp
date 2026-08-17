// SPDX-License-Identifier: GPL-3.0-or-later
//
// The geometry, against shapes whose answers are known before the code runs.
//
// A circular arc is the useful case: its curvature radius is the radius, its
// length is r*theta, and both are known in closed form -- so the test cannot
// agree with the implementation by construction the way a hand-computed
// expected value copied out of a debugger would.

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "path.h"

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

// Irvine, the coordinate docs/map.md uses for everything in the SoCal archive.
constexpr double kIrvineLat = 33.6866;
constexpr double kIrvineLon = -117.8558;

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// A circular arc of `radiusM`, centred near Irvine, as interleaved lon/lat E7.
//
// Latitude and longitude are scaled differently on purpose: a degree of
// longitude is shorter than a degree of latitude by cos(lat), and an arc built
// without that correction is an ellipse -- whose curvature is not its "radius",
// which would make this test assert the wrong thing.
std::vector<std::int32_t> arcLonLat(double radiusM, double sweepRad, std::size_t points)
{
    constexpr double kMetresPerDegreeLat = 111132.0;
    const double cosLat = std::cos(kIrvineLat / kDegPerRad);

    std::vector<std::int32_t> out;
    out.reserve(points * 2);

    for (std::size_t i = 0; i < points; ++i)
    {
        const double t = (points == 1) ? 0.0 : (static_cast<double>(i) /
                                                static_cast<double>(points - 1));
        const double angle = t * sweepRad;

        const double northM = radiusM * std::sin(angle);
        const double eastM = radiusM * (1.0 - std::cos(angle));

        const double lat = kIrvineLat + (northM / kMetresPerDegreeLat);
        const double lon = kIrvineLon + (eastM / (kMetresPerDegreeLat * cosLat));

        out.push_back(road_graph::fromDegrees(lon));
        out.push_back(road_graph::fromDegrees(lat));
    }

    return out;
}

// ============================================================================

// The single most valuable test in this file. Both map sources hand out
// interleaved LON, LAT while everything on the bus is lat-first, and a swap
// produces two entirely valid numbers describing a point 117 degrees away.
void test_geometry_is_decoded_lon_first()
{
    const std::vector<std::int32_t> lonLat {
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon + 0.001), road_graph::fromDegrees(kIrvineLat + 0.001),
    };

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem),
          "a two-point lon/lat list decodes");

    check(path.size() == 2, "both points survive");
    checkNear(road_graph::toDegrees(path.lat[0]), kIrvineLat, 1e-6,
              "latitude comes from the SECOND value of each pair");
    checkNear(road_graph::toDegrees(path.lon[0]), kIrvineLon, 1e-6,
              "longitude comes from the FIRST value of each pair");

    // The failure this guards against, stated positively: latitude is positive
    // and longitude is negative in California, so a swap is a sign flip.
    check(path.lat[0] > 0 && path.lon[0] < 0, "the point is in California, not the Indian Ocean");
}

void test_odd_and_short_geometry_is_refused()
{
    Path path;
    std::string problem;

    const std::vector<std::int32_t> odd { 1, 2, 3 };
    check(!bd992_mock::setGeometryFromLonLat(odd, path, problem),
          "an odd-length list is refused rather than truncated");
    check(!problem.empty(), "and says why");

    const std::vector<std::int32_t> single { road_graph::fromDegrees(kIrvineLon),
                                             road_graph::fromDegrees(kIrvineLat) };
    check(!bd992_mock::setGeometryFromLonLat(single, path, problem),
          "a one-point path is refused: there is no direction to drive in");
}

void test_distances_match_the_geometry()
{
    // Three points 100 m apart along a meridian, where a degree of latitude is
    // a known length.
    constexpr double kMetresPerDegreeLat = 111132.0;
    const double step = 100.0 / kMetresPerDegreeLat;

    std::vector<std::int32_t> lonLat;
    for (int i = 0; i < 3; ++i)
    {
        lonLat.push_back(road_graph::fromDegrees(kIrvineLon));
        lonLat.push_back(road_graph::fromDegrees(kIrvineLat + (step * i)));
    }

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem), "the meridian decodes");
    bd992_mock::computeDistances(path);

    checkNear(path.distanceM[0], 0.0, 1e-9, "distance starts at zero");
    checkNear(path.distanceM[1], 100.0, 0.5, "100 m to the second point");
    checkNear(path.distanceM[2], 200.0, 0.5, "200 m to the third");
    checkNear(path.lengthM(), 200.0, 0.5, "an open path is as long as its last distance");
}

void test_a_closed_path_includes_the_closing_leg()
{
    // A square, 100 m on a side.
    constexpr double kMetresPerDegreeLat = 111132.0;
    const double dLat = 100.0 / kMetresPerDegreeLat;
    const double dLon = 100.0 / (kMetresPerDegreeLat * std::cos(kIrvineLat / kDegPerRad));

    const std::vector<std::int32_t> lonLat {
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon + dLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon + dLon), road_graph::fromDegrees(kIrvineLat + dLat),
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat + dLat),
    };

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem), "the square decodes");
    bd992_mock::computeDistances(path);

    path.closed = false;
    checkNear(path.lengthM(), 300.0, 1.0, "open, the square is three sides long");

    path.closed = true;
    checkNear(path.lengthM(), 400.0, 1.0, "closed, the fourth side counts");
}

void test_repeated_points_are_dropped()
{
    // docs/tracks.md: 87 of the 994 track outlines carry runs of identical
    // consecutive vertices, one of them 2 346 long.
    const std::vector<std::int32_t> lonLat {
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon), road_graph::fromDegrees(kIrvineLat),
        road_graph::fromDegrees(kIrvineLon + 0.001), road_graph::fromDegrees(kIrvineLat),
    };

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem), "the stuttering path decodes");
    bd992_mock::dropRepeatedPoints(path);

    check(path.size() == 2, "a run of identical vertices collapses to one");
}

void test_curvature_recovers_the_radius_of_an_arc()
{
    for (const double radius : { 30.0, 100.0, 500.0 })
    {
        Path path;
        std::string problem;
        check(bd992_mock::setGeometryFromLonLat(arcLonLat(radius, std::numbers::pi / 2.0, 40), path,
                                                problem),
              "the arc decodes");
        bd992_mock::computeDistances(path);

        // Away from the ends, where an open path has no road to measure over.
        const double measured = bd992_mock::curvatureRadiusM(path, path.size() / 2, 8.0);
        checkNear(measured, radius, radius * 0.02,
                  "curvature recovers the radius of a " + std::to_string(radius) + " m arc");
    }
}

// The reason curvatureRadiusM takes a baseline at all. A 30 m corner sampled
// every 30 cm bows 0.4 mm off the chord between adjacent vertices, against an
// 11 mm coordinate grid -- so the adjacent-vertex circle is fitted to rounding.
// Widening the baseline must recover the real radius from the same points.
void test_curvature_survives_dense_sampling()
{
    constexpr double radius = 30.0;

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(arcLonLat(radius, std::numbers::pi / 2.0, 160), path,
                                            problem),
          "the densely sampled arc decodes");
    bd992_mock::computeDistances(path);

    const std::size_t middle = path.size() / 2;

    const double wide = bd992_mock::curvatureRadiusM(path, middle, 8.0);
    checkNear(wide, radius, radius * 0.02,
              "an 8 m baseline recovers the radius however densely the arc is sampled");

    // And the pathology it exists to avoid, stated as a fact about the data
    // rather than an assertion about behaviour: over a baseline below the
    // grid's reach the answer is not trustworthy. Averaged over the arc so the
    // test does not hinge on one unlucky triple.
    double worstNarrowError = 0.0;
    for (std::size_t i = 1; i + 1 < path.size(); ++i)
    {
        const double narrow = bd992_mock::curvatureRadiusM(path, i, 0.0);
        if (std::isfinite(narrow))
        {
            worstNarrowError = std::max(worstNarrowError, std::fabs(narrow - radius) / radius);
        }
    }
    check(worstNarrowError > 0.05,
          "an adjacent-vertex baseline really is noisy on this data, so the wide one is earning "
          "its keep");
}

void test_curvature_declines_to_guess_at_the_ends_of_an_open_path()
{
    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(arcLonLat(50.0, std::numbers::pi / 2.0, 40), path,
                                            problem),
          "the arc decodes");
    bd992_mock::computeDistances(path);

    check(!std::isfinite(bd992_mock::curvatureRadiusM(path, 0, 8.0)),
          "the first vertex of an open path has no road behind it to measure over");
    check(!std::isfinite(bd992_mock::curvatureRadiusM(path, path.size() - 1, 8.0)),
          "and the last has none ahead");
}

void test_a_straight_line_has_no_curvature()
{
    constexpr double kMetresPerDegreeLat = 111132.0;
    std::vector<std::int32_t> lonLat;
    for (int i = 0; i < 5; ++i)
    {
        lonLat.push_back(road_graph::fromDegrees(kIrvineLon));
        lonLat.push_back(road_graph::fromDegrees(kIrvineLat + (i * 50.0 / kMetresPerDegreeLat)));
    }

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem), "the straight decodes");
    bd992_mock::computeDistances(path);

    const double radius = bd992_mock::curvatureRadiusM(path, 2, 8.0);
    check(radius > 1e5, "a straight line's circumradius is enormous, so nothing caps the speed");
}

void test_arc_length_matches_r_theta()
{
    constexpr double radius = 200.0;
    constexpr double sweep = std::numbers::pi / 2.0;

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(arcLonLat(radius, sweep, 200), path, problem),
          "the quarter circle decodes");
    bd992_mock::computeDistances(path);

    // Chorded rather than curved, so the polyline is very slightly short.
    checkNear(path.lengthM(), radius * sweep, radius * sweep * 0.001,
              "a quarter circle of radius 200 is r*theta long");
}

void test_sampling_walks_along_the_path()
{
    constexpr double kMetresPerDegreeLat = 111132.0;
    std::vector<std::int32_t> lonLat;
    for (int i = 0; i < 11; ++i)
    {
        lonLat.push_back(road_graph::fromDegrees(kIrvineLon));
        lonLat.push_back(road_graph::fromDegrees(kIrvineLat + (i * 100.0 / kMetresPerDegreeLat)));
    }

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(lonLat, path, problem), "the 1 km straight decodes");
    bd992_mock::computeDistances(path);

    // The path's own measured length, not the nominal 1000: it is built from a
    // metres-per-degree constant and measured with road_graph's, which differ
    // in the fourth digit.
    const double length = path.lengthM();

    const bd992_mock::PathPoint start = bd992_mock::samplePath(path, 0.0, 5.0);
    const bd992_mock::PathPoint middle = bd992_mock::samplePath(path, 500.0, 5.0);
    const bd992_mock::PathPoint end = bd992_mock::samplePath(path, length, 5.0);

    checkNear(road_graph::distanceM(start.lat, start.lon, middle.lat, middle.lon), 500.0, 1.0,
              "sampling at 500 m is 500 m from the start");
    checkNear(road_graph::distanceM(start.lat, start.lon, end.lat, end.lon), length, 1.0,
              "sampling at the far end is the far end");

    // Due north, and still due north at the very end where there is no road
    // left to look along.
    checkNear(middle.headingDeg, 0.0, 1.0, "heading along a northward straight is 0 degrees");
    checkNear(end.headingDeg, 0.0, 1.0,
              "the last point keeps pointing along the road rather than defaulting to north");

    // Past the end of an open path, position clamps rather than running on.
    const bd992_mock::PathPoint past = bd992_mock::samplePath(path, 5000.0, 5.0);
    checkNear(road_graph::distanceM(past.lat, past.lon, end.lat, end.lon), 0.0, 0.5,
              "an open path clamps at its end");
}

void test_a_closed_path_wraps_without_a_jump()
{
    // A full circle, so that wrapping past the seam is a continuation rather
    // than a corner.
    constexpr double radius = 150.0;

    Path path;
    std::string problem;
    check(bd992_mock::setGeometryFromLonLat(
              arcLonLat(radius, 2.0 * std::numbers::pi * 0.995, 360), path, problem),
          "the circle decodes");
    bd992_mock::computeDistances(path);
    path.closed = true;

    const double length = path.lengthM();

    // Just before the wrap and just after it should be a small step apart, not
    // a leap back to the start.
    const bd992_mock::PathPoint before = bd992_mock::samplePath(path, length - 0.5, 5.0);
    const bd992_mock::PathPoint after = bd992_mock::samplePath(path, length + 0.5, 5.0);

    const double step = road_graph::distanceM(before.lat, before.lon, after.lat, after.lon);
    check(step < 5.0, "position is continuous across the seam of a closed path");

    const double turn = road_graph::bearingDeltaDeg(before.headingDeg, after.headingDeg);
    check(turn < 20.0, "heading is continuous across the seam too");

    // And a full lap returns to where it started.
    const bd992_mock::PathPoint origin = bd992_mock::samplePath(path, 0.0, 5.0);
    const bd992_mock::PathPoint lapped = bd992_mock::samplePath(path, length, 5.0);
    checkNear(road_graph::distanceM(origin.lat, origin.lon, lapped.lat, lapped.lon), 0.0, 0.5,
              "one lap along a closed path is back at the start");
}

} // namespace

int main()
{
    // No level squelch here: nothing in this file logs an expected error, so a
    // failed check must be able to say which one it was.
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_geometry_is_decoded_lon_first();
    test_odd_and_short_geometry_is_refused();
    test_distances_match_the_geometry();
    test_a_closed_path_includes_the_closing_leg();
    test_repeated_points_are_dropped();
    test_curvature_recovers_the_radius_of_an_arc();
    test_curvature_survives_dense_sampling();
    test_curvature_declines_to_guess_at_the_ends_of_an_open_path();
    test_a_straight_line_has_no_curvature();
    test_arc_length_matches_r_theta();
    test_sampling_walks_along_the_path();
    test_a_closed_path_wraps_without_a_jump();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all bd992_mock path checks passed");
    return 0;
}
