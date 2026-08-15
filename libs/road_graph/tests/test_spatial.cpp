// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding the road under a position.
//
// This is what map/nearest answers and what the matcher stands on, and the
// interesting case is the one distance alone gets wrong. A freeway and its
// frontage road run parallel thirty metres apart; a fix between them is nearer
// whichever way the noise fell. What separates them is HEADING -- and a matcher
// that ignores it puts the vehicle on the frontage road while the map still
// renders perfectly.

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "road_graph/builder.h"
#include "road_graph/graph.h"

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

// At this latitude 1e-7 degrees of latitude is about 1.1 cm, so 1000 units is
// roughly 11 m. Written out because every number below is derived from it.
constexpr road_graph::Coord kAbout11m = 1000;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

map_rules::RoadClassification road(map_rules::RouteClass routeClass)
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Minor;
    out.routeClass = routeClass;
    out.access = map_rules::kAccessMotorcar;
    out.freeFlowSpeedKph = 50;
    return out;
}

// A straight line of `points` running north from (lat, lon).
road_graph::Builder::SegmentInput northward(std::int64_t wayId, std::int64_t fromNode,
                                            std::int64_t toNode, road_graph::Coord lat,
                                            road_graph::Coord lon, const std::string& name)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, 0);
    input.osmWayId = wayId;
    input.fromNodeId = fromNode;
    input.toNodeId = toNode;
    for (int i = 0; i < 5; ++i)
    {
        input.geometry.push_back(lat + i * kAbout11m);
        input.geometry.push_back(lon);
    }
    input.classification = road(map_rules::RouteClass::Minor);
    input.name = name;
    return input;
}

road_graph::Builder::SegmentInput eastward(std::int64_t wayId, std::int64_t fromNode,
                                           std::int64_t toNode, road_graph::Coord lat,
                                           road_graph::Coord lon, const std::string& name)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, 0);
    input.osmWayId = wayId;
    input.fromNodeId = fromNode;
    input.toNodeId = toNode;
    for (int i = 0; i < 5; ++i)
    {
        input.geometry.push_back(lat);
        input.geometry.push_back(lon + i * kAbout11m);
    }
    input.classification = road(map_rules::RouteClass::Minor);
    input.name = name;
    return input;
}

void test_the_nearest_road_is_found()
{
    const auto path = scratch("road_graph_nearest.graph");

    road_graph::Builder builder;
    builder.add(northward(1, 1, 2, kLat, kLon, "Main Street"));
    builder.add(northward(2, 3, 4, kLat, kLon + 100 * kAbout11m, "Far Away Road"));
    check(builder.write(path, 0).has_value(), "two roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // A point right on Main Street.
    const auto matches = graph->nearest(kLat + 2 * kAbout11m, kLon, 50.0, 4);
    check(!matches.empty(), "a point on a road matches something");
    if (matches.empty())
    {
        return;
    }

    const auto& segment = graph->segments()[matches[0].segment];
    check(graph->nameOf(segment) == "Main Street", "and it is the road it is on");
    check(matches[0].distanceM < 1.0, "at essentially zero distance");
    check(matches[0].offsetCm > 0, "with an offset along the road");

    std::filesystem::remove(path);
}

void test_nothing_within_the_radius_matches_nothing()
{
    // A car park, a private drive, the middle of a field. NORMAL, and it has to
    // come back empty rather than as the nearest road a kilometre away.
    const auto path = scratch("road_graph_nomatch.graph");

    road_graph::Builder builder;
    builder.add(northward(1, 1, 2, kLat, kLon, "Main Street"));
    check(builder.write(path, 0).has_value(), "a road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    const auto matches = graph->nearest(kLat, kLon + 1000 * kAbout11m, 50.0, 4);
    check(matches.empty(), "a point far from any road matches nothing");

    std::filesystem::remove(path);
}

void test_heading_separates_a_road_from_the_one_beside_it()
{
    // THE case. Two roads 22 m apart, one running north and one running east,
    // and a fix almost exactly between them. Distance alone is a coin toss;
    // heading decides it.
    const auto path = scratch("road_graph_heading.graph");

    road_graph::Builder builder;
    builder.add(northward(1, 1, 2, kLat, kLon, "Northbound"));
    builder.add(eastward(2, 3, 4, kLat + 2 * kAbout11m, kLon - 2 * kAbout11m, "Eastbound"));
    check(builder.write(path, 0).has_value(), "two crossing roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // A point near where they meet.
    const road_graph::Coord lat = kLat + 2 * kAbout11m;
    const road_graph::Coord lon = kLon - kAbout11m / 2;

    const auto goingNorth = graph->nearest(lat, lon, 60.0, 4, 0.0);
    check(!goingNorth.empty(), "travelling north matches something");
    if (!goingNorth.empty())
    {
        const auto& segment = graph->segments()[goingNorth[0].segment];
        check(graph->nameOf(segment) == "Northbound",
              "and picks the road running the way we are going");
    }

    const auto goingEast = graph->nearest(lat, lon, 60.0, 4, 90.0);
    check(!goingEast.empty(), "travelling east matches something");
    if (!goingEast.empty())
    {
        const auto& segment = graph->segments()[goingEast[0].segment];
        check(graph->nameOf(segment) == "Eastbound",
              "and picks the OTHER road, from the same position");
    }

    std::filesystem::remove(path);
}

void test_heading_is_direction_agnostic()
{
    // A road may be travelled either way. Driving south down a north-south road
    // must still match it, or every return trip loses the road it is on.
    const auto path = scratch("road_graph_reverse.graph");

    road_graph::Builder builder;
    builder.add(northward(1, 1, 2, kLat, kLon, "Northbound"));
    builder.add(eastward(2, 3, 4, kLat + 2 * kAbout11m, kLon - 2 * kAbout11m, "Eastbound"));
    check(builder.write(path, 0).has_value(), "two roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    const auto matches = graph->nearest(kLat + 2 * kAbout11m, kLon - kAbout11m / 2, 60.0, 4, 180.0);
    check(!matches.empty(), "travelling south matches something");
    if (!matches.empty())
    {
        const auto& segment = graph->segments()[matches[0].segment];
        check(graph->nameOf(segment) == "Northbound",
              "and it is still the north-south road, driven the other way");
    }

    std::filesystem::remove(path);
}

void test_candidates_come_back_nearest_first()
{
    const auto path = scratch("road_graph_order.graph");

    road_graph::Builder builder;
    builder.add(northward(1, 1, 2, kLat, kLon, "Near"));
    builder.add(northward(2, 3, 4, kLat, kLon + 2 * kAbout11m, "Middle"));
    builder.add(northward(3, 5, 6, kLat, kLon + 4 * kAbout11m, "Far"));
    check(builder.write(path, 0).has_value(), "three parallel roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    const auto matches = graph->nearest(kLat + 2 * kAbout11m, kLon, 200.0, 3);
    check(matches.size() == 3, "all three are within the radius");
    if (matches.size() != 3)
    {
        return;
    }

    check(graph->nameOf(graph->segments()[matches[0].segment]) == "Near", "nearest first");
    check(matches[0].distanceM <= matches[1].distanceM, "then in increasing distance");
    check(matches[1].distanceM <= matches[2].distanceM, "all the way down");

    const auto limited = graph->nearest(kLat + 2 * kAbout11m, kLon, 200.0, 1);
    check(limited.size() == 1, "and maxCandidates is respected");

    std::filesystem::remove(path);
}

void test_an_unroutable_segment_is_never_a_match()
{
    // A river passes under the road. Matching onto it would put the vehicle in
    // the water and give the horizon a road class of "waterway".
    const auto path = scratch("road_graph_river_match.graph");

    map_rules::RoadClassification river;
    river.renderClass = map_rules::RenderClass::Waterway;
    river.routeClass = map_rules::RouteClass::None;

    road_graph::Builder builder;
    auto water = northward(1, 1, 2, kLat, kLon, "Santa Ana River");
    water.classification = river;
    builder.add(std::move(water));
    builder.add(northward(2, 3, 4, kLat, kLon + 4 * kAbout11m, "Road"));
    check(builder.write(path, 0).has_value(), "a river and a road write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // Standing ON the river.
    const auto matches = graph->nearest(kLat + 2 * kAbout11m, kLon, 200.0, 4);
    check(!matches.empty(), "something still matches");
    for (const auto& match : matches)
    {
        check(graph->nameOf(graph->segments()[match.segment]) != "Santa Ana River",
              "and it is never the river");
    }

    std::filesystem::remove(path);
}

void test_a_bigger_road_wins_a_close_call()
{
    // Distance alone puts a vehicle on whatever is nearest, and in a business
    // park that is a parking aisle rather than the road it is driving on. A
    // modest class bias fixes that -- and must NOT be so strong that a car
    // genuinely in a car park stops matching the car park.
    const auto path = scratch("road_graph_class_bias.graph");

    road_graph::Builder builder;

    auto aisle = northward(1, 1, 2, kLat, kLon, "Parking Aisle");
    aisle.classification.routeClass = map_rules::RouteClass::Service;
    builder.add(std::move(aisle));

    // A primary road, 55 m east of the aisle. Far enough apart that a point can
    // be decisively on one of them, which is what makes the two cases below
    // distinguishable at all.
    auto primary = northward(2, 3, 4, kLat, kLon + 5 * kAbout11m, "Alton Parkway");
    primary.classification.routeClass = map_rules::RouteClass::Primary;
    builder.add(std::move(primary));

    check(builder.write(path, 0).has_value(), "an aisle and a primary road write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // Between them, and NEARER THE AISLE -- 22 m from it against 33 m from the
    // road. Distance alone picks the aisle; the bias picks the road.
    const auto between = graph->nearest(kLat + 2 * kAbout11m, kLon + 2 * kAbout11m, 60.0, 4);
    check(!between.empty(), "a point between them matches");
    if (!between.empty())
    {
        check(graph->nameOf(graph->segments()[between[0].segment]) == "Alton Parkway",
              "and the bigger road wins a close call");
    }

    // Squarely ON the aisle, 55 m from the primary. The bias must not reach this
    // far, or a car in a car park is told it is on the road outside. That it
    // takes roughly 18 m of margin to flip is the calibration this pins.
    const auto onAisle = graph->nearest(kLat + 2 * kAbout11m, kLon, 80.0, 4);
    check(!onAisle.empty(), "a point on the aisle matches");
    if (!onAisle.empty())
    {
        check(graph->nameOf(graph->segments()[onAisle[0].segment]) == "Parking Aisle",
              "and a clear distance answer still wins");
    }

    std::filesystem::remove(path);
}

void test_a_box_query_finds_what_is_in_it()
{
    const auto path = scratch("road_graph_box.graph");

    road_graph::Builder builder;
    for (int i = 0; i < 40; ++i)
    {
        builder.add(northward(100 + i, 1000 + i * 2, 1001 + i * 2, kLat + i * 10 * kAbout11m, kLon,
                              "Road " + std::to_string(i)));
    }
    check(builder.write(path, 0).has_value(), "forty roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // A box around the first few. The R-tree has several levels at this size,
    // so this exercises the descent rather than a single leaf scan.
    std::vector<road_graph::SegmentIndex> found;
    graph->queryBox(kLon - kAbout11m, kLat - kAbout11m, kLon + kAbout11m,
                    kLat + 25 * kAbout11m,
                    [&](road_graph::SegmentIndex index) { found.push_back(index); });

    check(!found.empty(), "a box query finds something");
    check(found.size() < 40, "and not everything -- the tree is pruning");

    // Everything it returned must actually intersect the box.
    bool allInside = true;
    for (const road_graph::SegmentIndex index : found)
    {
        const auto& segment = graph->segments()[index];
        const auto geometry = graph->geometryOf(segment);
        bool intersects = false;
        for (std::size_t i = 0; i + 1 < geometry.size(); i += 2)
        {
            if (geometry[i] >= kLat - kAbout11m && geometry[i] <= kLat + 25 * kAbout11m)
            {
                intersects = true;
            }
        }
        if (!intersects)
        {
            allInside = false;
        }
    }
    check(allInside, "and everything it returned really is in range");

    std::filesystem::remove(path);
}

void test_a_large_graph_is_searchable()
{
    // Enough segments that the packed tree is several levels deep, so a bug in
    // the level arithmetic shows up as a miss rather than as a slow query.
    const auto path = scratch("road_graph_large.graph");

    road_graph::Builder builder;
    for (int i = 0; i < 500; ++i)
    {
        const road_graph::Coord lat = kLat + (i % 25) * 20 * kAbout11m;
        const road_graph::Coord lon = kLon + (i / 25) * 20 * kAbout11m;
        builder.add(northward(1000 + i, 5000 + i * 2, 5001 + i * 2, lat, lon,
                              "Grid " + std::to_string(i)));
    }
    check(builder.write(path, 0).has_value(), "a five-hundred segment grid writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // Every one of them must be findable from a point on it.
    std::size_t missed = 0;
    for (std::uint32_t i = 0; i < graph->header().segmentCount; ++i)
    {
        const auto& segment = graph->segments()[i];
        const auto geometry = graph->geometryOf(segment);
        const auto matches = graph->nearest(geometry[2], geometry[3], 30.0, 4);

        bool found = false;
        for (const auto& match : matches)
        {
            if (match.segment == i)
            {
                found = true;
            }
        }
        if (!found)
        {
            ++missed;
        }
    }
    check(missed == 0, "every segment in a 500-segment graph is findable from a point on it");
    if (missed != 0)
    {
        SPDLOG_ERROR("  {} segments were missed", missed);
    }

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_nearest_road_is_found();
    test_nothing_within_the_radius_matches_nothing();
    test_heading_separates_a_road_from_the_one_beside_it();
    test_heading_is_direction_agnostic();
    test_candidates_come_back_nearest_first();
    test_an_unroutable_segment_is_never_a_match();
    test_a_bigger_road_wins_a_close_call();
    test_a_box_query_finds_what_is_in_it();
    test_a_large_graph_is_searchable();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all spatial checks passed");
    return 0;
}
