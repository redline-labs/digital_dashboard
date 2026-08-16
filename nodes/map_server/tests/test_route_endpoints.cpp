// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which junctions a route search actually runs between.
//
// The bug this exists for is quiet: handleRoute passed the departure heading
// into nearest() so it would rank the right carriageway first, and then routed
// from the segment's stored `toNode` regardless. A vehicle pointing against
// the stored direction got a route beginning at the junction BEHIND it -- so
// the route opens with a U-turn the driver did not make, and it is longest and
// most wrong on exactly the divided carriageways where supplying a heading was
// the point.

#include "route_endpoints.h"

#include "road_graph/builder.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
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

constexpr road_graph::Coord kLat = 336865966;
constexpr road_graph::Coord kLon = -1178557874;
// About 110 m per step at this latitude.
constexpr road_graph::Coord kStep = 10000;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

// One straight south-to-north road of two segments, so there is a middle
// junction and a real length to be partway along.
bool writeRoad(const std::filesystem::path& path)
{
    road_graph::Builder builder;

    for (int i = 0; i < 2; ++i)
    {
        road_graph::Builder::SegmentInput input;
        input.id = road_graph::makeSegmentId(1, static_cast<std::uint32_t>(i));
        input.osmWayId = 1;
        input.fromNodeId = i + 1;
        input.toNodeId = i + 2;
        input.geometry = { static_cast<road_graph::Coord>(kLat + i * kStep), kLon,
                           static_cast<road_graph::Coord>(kLat + (i + 1) * kStep), kLon };
        input.classification.renderClass = map_rules::RenderClass::Minor;
        input.classification.routeClass = map_rules::RouteClass::Minor;
        input.classification.access = map_rules::kAccessMotorcar;
        input.classification.freeFlowSpeedKph = 50;
        input.name = "Main Street";
        builder.add(std::move(input));
    }

    return builder.write(path, 0).has_value();
}

void test_the_heading_decides_which_end_the_route_starts_from()
{
    const auto path = scratch("ms_endpoints.graph");
    if (!writeRoad(path))
    {
        check(false, "the test graph writes");
        return;
    }

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "the test graph opens");
        std::filesystem::remove(path);
        return;
    }

    // Halfway up the first segment, so both ends are a real distance away and
    // the two directions cannot accidentally agree.
    const auto here = static_cast<road_graph::Coord>(kLat + kStep / 2);
    const auto snapped = graph->nearest(here, kLon, 200.0, 1);
    check(!snapped.empty(), "the position snaps to the road");
    if (snapped.empty())
    {
        std::filesystem::remove(path);
        return;
    }

    const road_graph::Match& match = snapped[0];
    const road_graph::SegmentRecord& segment = graph->segments()[match.segment];

    // The road runs south to north, so a bearing near 0 is "with" it.
    const auto northbound = map_server::resolveEndpoints(*graph, match, match, 0.0);
    const auto southbound = map_server::resolveEndpoints(*graph, match, match, 180.0);

    check(northbound.startForward, "driving north along a northward road is forward");
    check(!southbound.startForward, "and driving south along it is not");

    check(northbound.startNode == segment.toNode,
          "a northbound vehicle sets off towards the junction ahead of it");
    check(southbound.startNode == segment.fromNode,
          "and a southbound one towards the junction ahead of IT -- the other end");
    check(northbound.startNode != southbound.startNode,
          "which are not the same junction, or this test proves nothing");

    // The two remainders must sum to the segment: whichever way you face, what
    // is ahead plus what is behind is the whole road.
    const double whole = segment.lengthCm / 100.0;
    check(std::abs((northbound.startRemainingM + southbound.startRemainingM) - whole) < 0.05,
          "what is ahead one way plus what is ahead the other is the whole segment");
    check(northbound.startRemainingM > 1.0 && southbound.startRemainingM > 1.0,
          "and neither is zero from a midpoint");

    std::filesystem::remove(path);
}

void test_with_no_heading_the_segment_direction_stands()
{
    // Same rule handleNearest uses. There is nothing to know, so guessing
    // would be worse than the convention.
    const auto path = scratch("ms_endpoints_noheading.graph");
    if (!writeRoad(path))
    {
        check(false, "the test graph writes");
        return;
    }

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "the test graph opens");
        std::filesystem::remove(path);
        return;
    }

    const auto snapped =
        graph->nearest(static_cast<road_graph::Coord>(kLat + kStep / 2), kLon, 200.0, 1);
    if (snapped.empty())
    {
        check(false, "the position snaps to the road");
        std::filesystem::remove(path);
        return;
    }

    const auto ends = map_server::resolveEndpoints(*graph, snapped[0], snapped[0], std::nullopt);
    check(ends.startForward, "with no heading the segment's own direction stands");
    check(ends.startNode == graph->segments()[snapped[0].segment].toNode,
          "and the route sets off towards its toNode");

    std::filesystem::remove(path);
}

void test_the_partial_pieces_are_never_negative()
{
    // offsetCm accumulates haversine leg lengths while lengthCm is the
    // builder's total, so at the very end of a segment the two can disagree by
    // a centimetre. These are unsigned, so an unclamped subtraction there is
    // not a small error -- it is a 42 000 km one.
    const auto path = scratch("ms_endpoints_clamp.graph");
    if (!writeRoad(path))
    {
        check(false, "the test graph writes");
        return;
    }

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "the test graph opens");
        std::filesystem::remove(path);
        return;
    }

    road_graph::Match match;
    match.segment = 0;
    match.bearingDeg = 0.0;
    // Deliberately past the end.
    match.offsetCm = graph->segments()[0].lengthCm + 5000;

    const auto ends = map_server::resolveEndpoints(*graph, match, match, 0.0);
    const double whole = graph->segments()[0].lengthCm / 100.0;
    check(ends.startRemainingM >= 0.0 && ends.startRemainingM <= whole,
          "an offset past the end of the segment clamps rather than wrapping");
    check(ends.endLeadInM >= 0.0 && ends.endLeadInM <= whole, "and so does the destination's");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_heading_decides_which_end_the_route_starts_from();
    test_with_no_heading_the_segment_direction_stands();
    test_the_partial_pieces_are_never_negative();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all route endpoint checks passed");
    return 0;
}
