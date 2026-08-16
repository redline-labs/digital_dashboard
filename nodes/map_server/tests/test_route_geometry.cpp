// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thinning a route's polyline, and the two things that must survive it.
//
// Simplification is easy to get almost right: a version that drops the wrong
// points still produces a line that looks like a road at the zoom you were
// testing at, and only goes wrong somewhere else. The properties worth pinning
// are structural rather than visual -- the segment boundaries still line up,
// and nothing moves further than the caller allowed.

#include "route_geometry.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>
#include <vector>

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

constexpr road_graph::Coord kLat = 336865966;
constexpr road_graph::Coord kLon = -1178557874;

// A straight run of `count` points heading east, with `wobbleCm` of noise on
// the ones in between so there is something to remove.
void appendRun(std::vector<road_graph::Coord>& geometry, int count, int wobble)
{
    for (int i = 0; i < count; ++i)
    {
        const bool ends = (i == 0 || i == count - 1);
        geometry.push_back(static_cast<road_graph::Coord>(kLat + (ends ? 0 : (i % 2 ? wobble : -wobble))));
        geometry.push_back(static_cast<road_graph::Coord>(kLon + i * 200));
    }
}

void test_zero_tolerance_keeps_everything()
{
    // The default. A caller that says nothing must get the geometry it would
    // have got before there was a knob.
    std::vector<road_graph::Coord> geometry;
    appendRun(geometry, 10, 50);
    const std::vector<std::uint32_t> starts { 0, 10 };

    const auto out = map_server::simplifyPerSegment(geometry, starts, 0.0);
    check(out.geometry == geometry, "a tolerance of zero passes the geometry through unchanged");
    check(out.segmentStarts == starts, "and the boundaries with it");
}

void test_boundaries_survive_and_still_describe_the_runs()
{
    // THE property. segmentStarts is what lets a client say which points
    // belong to which road; simplifying across a boundary, or forgetting to
    // renumber, leaves it pointing at the wrong places -- and the line still
    // draws, so nothing looks wrong.
    std::vector<road_graph::Coord> geometry;
    appendRun(geometry, 12, 300);
    const std::uint32_t firstRun = 12;
    appendRun(geometry, 8, 300);
    const std::vector<std::uint32_t> starts { 0, firstRun, firstRun + 8 };

    const auto out = map_server::simplifyPerSegment(geometry, starts, 50.0);

    check(out.segmentStarts.size() == starts.size(),
          "the number of boundaries is unchanged by simplification");
    check(out.segmentStarts.front() == 0, "the first run still starts at zero");
    check(out.segmentStarts.back() == out.geometry.size() / 2,
          "and the closing entry still equals the point count");

    for (std::size_t i = 0; i + 1 < out.segmentStarts.size(); ++i)
    {
        check(out.segmentStarts[i] < out.segmentStarts[i + 1],
              "every run keeps at least one point, run " + std::to_string(i));
        check(out.segmentStarts[i + 1] - out.segmentStarts[i] >= 2,
              "in fact both of its ends, run " + std::to_string(i));
    }

    check(out.geometry.size() < geometry.size(), "and something was actually removed");
}

void test_no_point_moves_further_than_the_tolerance()
{
    // What the tolerance MEANS. A caller asking for 20 m is saying it will
    // draw at a zoom where 20 m is invisible; if a dropped point was 100 m off
    // the line, the route now cuts a corner it should not.
    std::vector<road_graph::Coord> geometry;
    appendRun(geometry, 40, 400);
    const std::vector<std::uint32_t> starts { 0, 40 };

    constexpr double kTolerance = 20.0;
    const auto out = map_server::simplifyPerSegment(geometry, starts, kTolerance);

    // Every ORIGINAL point must be within the tolerance of the kept polyline.
    const std::size_t kept = out.geometry.size() / 2;
    check(kept >= 2, "at least the two ends survive");

    double worst = 0.0;
    for (std::size_t i = 0; i < geometry.size() / 2; ++i)
    {
        double nearest = 1e18;
        for (std::size_t k = 0; k + 1 < kept; ++k)
        {
            const double away = road_graph::projectOnto(geometry[i * 2], geometry[(i * 2) + 1],
                                                        out.geometry[k * 2],
                                                        out.geometry[(k * 2) + 1],
                                                        out.geometry[(k + 1) * 2],
                                                        out.geometry[((k + 1) * 2) + 1])
                                    .distanceM;
            nearest = std::min(nearest, away);
        }
        worst = std::max(worst, nearest);
    }

    // A little slack for the 1e-7 degree quantisation of the coordinates.
    check(worst <= kTolerance + 1.0,
          "no original point is further than the tolerance from the simplified line, worst " +
              std::to_string(worst) + " m");
}

void test_a_run_of_two_points_is_left_alone()
{
    // Most segments are short. There is nothing between the ends to drop, and
    // an off-by-one here would delete the road.
    std::vector<road_graph::Coord> geometry { kLat, kLon, kLat, kLon + 1000 };
    const std::vector<std::uint32_t> starts { 0, 2 };

    const auto out = map_server::simplifyPerSegment(geometry, starts, 100.0);
    check(out.geometry.size() == 4, "a two-point run keeps both points");
    check(out.segmentStarts == starts, "and its boundaries");
}

void test_empty_input_does_not_produce_nonsense()
{
    const auto out = map_server::simplifyPerSegment({}, {}, 10.0);
    check(out.geometry.empty(), "no geometry in, none out");
    check(out.segmentStarts.empty(), "and no boundaries invented");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_zero_tolerance_keeps_everything();
    test_boundaries_survive_and_still_describe_the_runs();
    test_no_point_moves_further_than_the_tolerance();
    test_a_run_of_two_points_is_left_alone();
    test_empty_input_does_not_produce_nonsense();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all route geometry checks passed");
    return 0;
}
