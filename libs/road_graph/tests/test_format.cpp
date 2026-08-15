// SPDX-License-Identifier: GPL-3.0-or-later
//
// Build a graph, read it back, and check the invariants that are silently wrong
// when they break.
//
// The one that matters most is IDENTITY SURVIVING A REBUILD. Everything
// downstream -- the horizon on the bus, a route reply, eventually a feature in
// a vector tile -- names a segment by id. If that id were the array position,
// then reordering the input (which the Hilbert sort does on every build, and
// which an OSM refresh does anyway) would silently repoint every id that was
// ever published. The test for it is to build the same roads twice in different
// orders and require the ids to mean the same thing.

#include <cstdio>
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

// Irvine, so the numbers are recognisable against the rest of the map stack.
constexpr road_graph::Coord kLat = 336865966;
constexpr road_graph::Coord kLon = -1178557874;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

map_rules::RoadClassification motorway()
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Motorway;
    out.routeClass = map_rules::RouteClass::Motorway;
    out.access = map_rules::kAccessMotorcar;
    out.freeFlowSpeedKph = 105;
    out.hasPosted = true;
    out.postedSpeedKph = 105;
    out.postedSource = map_rules::SpeedSource::Sign;
    out.minZoom = 4;
    return out;
}

map_rules::RoadClassification residential()
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Minor;
    out.routeClass = map_rules::RouteClass::Minor;
    out.access = map_rules::kAccessMotorcar | map_rules::kAccessFoot;
    out.freeFlowSpeedKph = 40;
    out.minZoom = 12;
    return out;
}

road_graph::Builder::SegmentInput straight(std::int64_t wayId, std::uint32_t ordinal,
                                           std::int64_t fromNode, std::int64_t toNode,
                                           road_graph::Coord lat, road_graph::Coord lon,
                                           road_graph::Coord dLat, road_graph::Coord dLon,
                                           const map_rules::RoadClassification& classification,
                                           const std::string& name)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, ordinal);
    input.osmWayId = wayId;
    input.fromNodeId = fromNode;
    input.toNodeId = toNode;
    input.geometry = { lat, lon, lat + dLat / 2, lon + dLon / 2, lat + dLat, lon + dLon };
    input.classification = classification;
    input.name = name;
    return input;
}

void test_a_graph_round_trips()
{
    const auto path = scratch("road_graph_round_trip.graph");

    road_graph::Builder builder;
    builder.add(straight(100, 0, 1, 2, kLat, kLon, 2000, 0, motorway(), "Costa Mesa Freeway"));
    builder.add(straight(100, 1, 2, 3, kLat + 2000, kLon, 2000, 0, motorway(), "Costa Mesa Freeway"));
    builder.add(straight(200, 0, 2, 4, kLat + 2000, kLon, 0, 2000, residential(), "Alton Parkway"));

    auto written = builder.write(path, 1755000000);
    check(written.has_value(), "a graph writes");
    if (!written)
    {
        SPDLOG_ERROR("  {}", road_graph::to_string(written.error()));
        return;
    }

    auto graph = road_graph::Graph::open(path);
    check(graph.has_value(), "and opens");
    if (!graph)
    {
        SPDLOG_ERROR("  {}", road_graph::to_string(graph.error()));
        return;
    }

    check(graph->header().segmentCount == 3, "with all three segments");
    check(graph->header().nodeCount == 4, "and four distinct junctions");
    check(graph->header().builtAtUnixS == 1755000000, "and the build time it was given");

    // Coverage should bracket the geometry that went in.
    check(graph->header().south <= kLat && graph->header().north >= kLat + 4000,
          "and a bounding box that covers the roads");

    std::filesystem::remove(path);
}

void test_ids_survive_a_rebuild_in_a_different_order()
{
    // THE test. Build the same roads twice, adding them in opposite orders, and
    // require that a SegmentId means the same road in both files -- even though
    // the Hilbert sort will have put them at different array positions.
    const auto first = scratch("road_graph_order_a.graph");
    const auto second = scratch("road_graph_order_b.graph");

    const auto build = [&](const std::filesystem::path& path, bool reversed) {
        road_graph::Builder builder;
        std::vector<road_graph::Builder::SegmentInput> inputs;
        inputs.push_back(
            straight(700, 0, 10, 11, kLat, kLon, 3000, 0, motorway(), "Freeway"));
        inputs.push_back(
            straight(800, 0, 20, 21, kLat + 50000, kLon + 50000, 0, 3000, residential(), "Elm"));
        inputs.push_back(
            straight(900, 0, 30, 31, kLat - 50000, kLon - 50000, 3000, 3000, residential(), "Oak"));

        if (reversed)
        {
            std::reverse(inputs.begin(), inputs.end());
        }
        for (auto& input : inputs)
        {
            builder.add(std::move(input));
        }
        return builder.write(path, 1755000000);
    };

    check(build(first, false).has_value(), "the first build writes");
    check(build(second, true).has_value(), "and the second, in the other order");

    auto a = road_graph::Graph::open(first);
    auto b = road_graph::Graph::open(second);
    check(a.has_value() && b.has_value(), "and both open");
    if (!a || !b)
    {
        return;
    }

    for (const std::int64_t wayId : { 700, 800, 900 })
    {
        const road_graph::SegmentId id = road_graph::makeSegmentId(wayId, 0);

        auto indexA = a->indexOf(id);
        auto indexB = b->indexOf(id);
        check(indexA.has_value() && indexB.has_value(),
              "way " + std::to_string(wayId) + " is findable by id in both");
        if (!indexA || !indexB)
        {
            continue;
        }

        const auto& segmentA = a->segments()[*indexA];
        const auto& segmentB = b->segments()[*indexB];

        check(segmentA.osmWayId == wayId && segmentB.osmWayId == wayId,
              "and resolves to the same source way in both");
        check(a->nameOf(segmentA) == b->nameOf(segmentB),
              "with the same name -- the id means the same ROAD, not the same slot");

        const auto geomA = a->geometryOf(segmentA);
        const auto geomB = b->geometryOf(segmentB);
        check(geomA.size() == geomB.size() && !geomA.empty(), "and the same geometry length");
        if (!geomA.empty() && geomA.size() == geomB.size())
        {
            check(geomA[0] == geomB[0] && geomA[1] == geomB[1],
                  "starting at the same place");
        }
    }

    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

void test_a_ways_segments_are_contiguous_and_ordered()
{
    // Decision 3. Stage 4 resolves a turn restriction's from-way to the piece
    // of it that touches the junction; that is only answerable if a way's
    // segments are a contiguous run in the file, in order along the way.
    const auto path = scratch("road_graph_way_index.graph");

    road_graph::Builder builder;
    // Added out of order on purpose: the builder has to put them back.
    builder.add(straight(500, 2, 3, 4, kLat + 4000, kLon, 2000, 0, residential(), "Long Road"));
    builder.add(straight(500, 0, 1, 2, kLat, kLon, 2000, 0, residential(), "Long Road"));
    builder.add(straight(500, 1, 2, 3, kLat + 2000, kLon, 2000, 0, residential(), "Long Road"));
    builder.add(straight(600, 0, 9, 10, kLat + 90000, kLon, 2000, 0, residential(), "Elsewhere"));

    check(builder.write(path, 0).has_value(), "a multi-segment way writes");

    auto graph = road_graph::Graph::open(path);
    check(graph.has_value(), "and opens");
    if (!graph)
    {
        return;
    }

    const auto pieces = graph->segmentsOfWay(500);
    check(pieces.size() == 3, "the way reports all three of its pieces");
    if (pieces.size() != 3)
    {
        return;
    }

    bool contiguous = true;
    for (std::size_t i = 1; i < pieces.size(); ++i)
    {
        if (pieces[i] != pieces[i - 1] + 1)
        {
            contiguous = false;
        }
    }
    check(contiguous, "and they are contiguous in the file");

    bool ordered = true;
    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& segment = graph->segments()[pieces[i]];
        if (road_graph::ordinalOf(segment.id) != i)
        {
            ordered = false;
        }
    }
    check(ordered, "and in order along the way, whatever order they were added in");

    check(graph->segmentsOfWay(999999).empty(), "and a way that is not there reports nothing");

    std::filesystem::remove(path);
}

void test_geometry_is_stored_once_and_direction_lives_on_the_edge()
{
    // Decision 4. A two-way road has two directed edges and ONE polyline; the
    // edge that runs against it says so with a flag. Storing a reversed copy
    // would double the coordinate array, and stage 4's edge expansion would
    // then multiply that by average degree.
    const auto path = scratch("road_graph_geometry.graph");

    road_graph::Builder builder;
    builder.add(straight(1, 0, 1, 2, kLat, kLon, 2000, 0, residential(), "Two Way"));
    check(builder.write(path, 0).has_value(), "a two-way road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    check(graph->header().segmentCount == 1, "as one segment");
    check(graph->header().edgeCount == 2, "with two directed edges");
    check(graph->header().geometryCount == 3, "and exactly one copy of its three points");

    const auto& segment = graph->segments()[0];
    const auto forwardEdges = graph->edgesFrom(segment.fromNode);
    const auto backwardEdges = graph->edgesFrom(segment.toNode);
    check(forwardEdges.size() == 1 && backwardEdges.size() == 1,
          "one edge leaving each end");
    if (forwardEdges.size() == 1 && backwardEdges.size() == 1)
    {
        check(forwardEdges[0].forward == 1, "the one from the start runs with the geometry");
        check(backwardEdges[0].forward == 0, "and the one from the end runs against it");
        check(forwardEdges[0].segment == backwardEdges[0].segment,
              "and both name the SAME segment");
    }

    std::filesystem::remove(path);
}

void test_oneway_produces_a_single_edge()
{
    const auto path = scratch("road_graph_oneway.graph");

    auto oneway = motorway();
    oneway.onewayForward = true;

    road_graph::Builder builder;
    builder.add(straight(1, 0, 1, 2, kLat, kLon, 2000, 0, oneway, "One Way"));
    check(builder.write(path, 0).has_value(), "a one-way road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    check(graph->header().edgeCount == 1, "with only one directed edge");
    const auto& segment = graph->segments()[0];
    check(graph->edgesFrom(segment.fromNode).size() == 1, "leaving its start");
    check(graph->edgesFrom(segment.toNode).empty(),
          "and nothing leaving its end -- a router cannot drive it backwards");

    std::filesystem::remove(path);
}

void test_an_unroutable_segment_is_stored_but_not_connected()
{
    // A river is drawn and is not routable. It must be in the file -- the tiler
    // needs it -- and must contribute no edges, or a route will swim.
    const auto path = scratch("road_graph_river.graph");

    map_rules::RoadClassification river;
    river.renderClass = map_rules::RenderClass::Waterway;
    river.routeClass = map_rules::RouteClass::None;
    river.minZoom = 10;

    road_graph::Builder builder;
    builder.add(straight(1, 0, 1, 2, kLat, kLon, 2000, 0, river, "Santa Ana River"));
    builder.add(straight(2, 0, 3, 4, kLat + 9000, kLon, 2000, 0, residential(), "Road"));
    check(builder.write(path, 0).has_value(), "a river and a road write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    check(graph->header().segmentCount == 2, "both are stored");
    check(graph->header().edgeCount == 2, "but only the road contributes edges");

    std::filesystem::remove(path);
}

void test_a_wrong_magic_or_version_is_refused()
{
    const auto path = scratch("road_graph_bad.graph");

    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        const char junk[128] = { 'N', 'O', 'T', 'A', 'G', 'R', 'A', 'P' };
        std::fwrite(junk, 1, sizeof(junk), file);
        std::fclose(file);
    }

    auto graph = road_graph::Graph::open(path);
    check(!graph.has_value(), "a file that is not a graph is refused");
    if (!graph)
    {
        check(graph.error().kind == road_graph::Error::Kind::NotAGraph, "as NotAGraph");
    }

    std::filesystem::remove(path);

    auto missing = road_graph::Graph::open(scratch("road_graph_does_not_exist.graph"));
    check(!missing.has_value(), "and a file that is not there is refused");
    if (!missing)
    {
        check(missing.error().kind == road_graph::Error::Kind::NotFound, "as NotFound");
    }
}

void test_segment_ids_pack_and_unpack()
{
    const road_graph::SegmentId id = road_graph::makeSegmentId(1'400'000'000, 37);
    check(road_graph::wayOf(id) == 1'400'000'000, "a way id survives packing");
    check(road_graph::ordinalOf(id) == 37, "and so does the ordinal");

    // The ordinal field must not bleed into the way id, or two pieces of one
    // way would resolve to different ways.
    const road_graph::SegmentId zero = road_graph::makeSegmentId(1'400'000'000, 0);
    check(road_graph::wayOf(zero) == road_graph::wayOf(id),
          "and both pieces of a way report the same way");
    check(zero != id, "while remaining distinct ids");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_graph_round_trips();
    test_ids_survive_a_rebuild_in_a_different_order();
    test_a_ways_segments_are_contiguous_and_ordered();
    test_geometry_is_stored_once_and_direction_lives_on_the_edge();
    test_oneway_produces_a_single_edge();
    test_an_unroutable_segment_is_stored_but_not_connected();
    test_a_wrong_magic_or_version_is_refused();
    test_segment_ids_pack_and_unpack();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all road graph format checks passed");
    return 0;
}
