// SPDX-License-Identifier: GPL-3.0-or-later
//
// A PBF in, segments out.
//
// Two behaviours here are silent when wrong and are the reason this test
// exists:
//
//   SPLITTING. A way becomes segments at its junctions and nowhere else. Split
//   too eagerly and the graph is full of degree-two nodes that cost memory and
//   slow every expansion; split too little and two roads that meet are not
//   connected, so a router simply never turns -- and the map still draws both
//   roads crossing.
//
//   DROPPING. A way with an unresolvable vertex is dropped WHOLE. Truncating it
//   instead leaves the graph disconnected at a seam, which is invisible: the
//   road is drawn, the route just never uses it.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_build/extract.h"
#include "pbf_builder.h"

namespace
{

using osm_test::Bytes;

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

constexpr std::int64_t kLat = 336865966;
constexpr std::int64_t kLon = -1178557874;

std::filesystem::path writePbf(const std::string& name, const Bytes& bytes)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return path;
}

// A whole file: header blob, a node block, a way block, and optionally a
// relation block. Relations come LAST, as a sorted file has them.
Bytes makeFile(const std::vector<osm_test::DenseNodeSpec>& nodes,
               const std::vector<osm_test::WaySpec>& ways,
               const std::vector<std::string>& strings,
               const std::vector<osm_test::RelationSpec>& relations = {})
{
    osm_test::HeaderBlockSpec header;
    header.optionalFeatures = { "Sort.Type_then_ID" };
    Bytes file = osm_test::framed("OSMHeader", osm_test::zlibBlob(osm_test::headerBlock(header)));

    osm_test::PrimitiveBlockSpec nodeBlock;
    nodeBlock.strings = strings;
    osm_test::GroupSpec nodeGroup;
    nodeGroup.dense = nodes;
    nodeBlock.groups.push_back(nodeGroup);
    const Bytes nodeBytes =
        osm_test::framed("OSMData", osm_test::zlibBlob(osm_test::primitiveBlock(nodeBlock)));
    file.insert(file.end(), nodeBytes.begin(), nodeBytes.end());

    osm_test::PrimitiveBlockSpec wayBlock;
    wayBlock.strings = strings;
    osm_test::GroupSpec wayGroup;
    wayGroup.ways = ways;
    wayBlock.groups.push_back(wayGroup);
    const Bytes wayBytes =
        osm_test::framed("OSMData", osm_test::zlibBlob(osm_test::primitiveBlock(wayBlock)));
    file.insert(file.end(), wayBytes.begin(), wayBytes.end());

    if (!relations.empty())
    {
        osm_test::PrimitiveBlockSpec relationBlock;
        relationBlock.strings = strings;
        osm_test::GroupSpec relationGroup;
        relationGroup.relations = relations;
        relationBlock.groups.push_back(relationGroup);
        const Bytes relationBytes = osm_test::framed(
            "OSMData", osm_test::zlibBlob(osm_test::primitiveBlock(relationBlock)));
        file.insert(file.end(), relationBytes.begin(), relationBytes.end());
    }

    return file;
}

struct Extracted
{
    map_build::ExtractStats stats;
    std::vector<road_graph::Builder::SegmentInput> segments;
    std::vector<road_graph::Builder::RestrictionInput> restrictions;
    bool ok { false };
};

Extracted run(const std::filesystem::path& path)
{
    Extracted out;
    map_build::ExtractOptions options;
    options.input = path;
    options.progressEvery = 0;

    auto stats = map_build::extract(
        options,
        [&out](road_graph::Builder::SegmentInput&& segment) {
            out.segments.push_back(std::move(segment));
        },
        [&out](const road_graph::Builder::RestrictionInput& restriction) {
            out.restrictions.push_back(restriction);
        });
    if (!stats)
    {
        SPDLOG_ERROR("extract failed: {}", osm::to_string(stats.error()));
        return out;
    }
    out.stats = *stats;
    out.ok = true;
    return out;
}

void test_a_single_road_becomes_one_segment()
{
    const std::vector<std::string> strings { "", "highway", "residential", "name", "Alton Parkway" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 4; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }

    std::vector<osm_test::WaySpec> ways { { 500, { 100, 101, 102, 103 }, { { 1, 2 }, { 3, 4 } } } };

    const auto path = writePbf("map_build_single.pbf", makeFile(nodes, ways, strings));
    const auto result = run(path);

    check(result.ok, "a one-way file extracts");
    if (!result.ok)
    {
        return;
    }

    check(result.segments.size() == 1,
          "a road with no junctions in the middle is ONE segment, not one per point");
    if (result.segments.size() == 1)
    {
        check(result.segments[0].geometry.size() == 8, "carrying all four of its points");
        check(result.segments[0].name == "Alton Parkway", "and its name");
        check(result.segments[0].osmWayId == 500, "and its source way");
        check(road_graph::ordinalOf(result.segments[0].id) == 0, "as ordinal 0");
    }

    std::filesystem::remove(path);
}

void test_a_way_splits_where_another_way_touches_it()
{
    // THE splitting case. Two roads crossing at a shared node: the through road
    // must become two segments so a router can turn onto the other one.
    const std::vector<std::string> strings { "", "highway", "residential" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    // The through road, north-south.
    for (int i = 0; i < 5; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }
    // The side road, running east from the middle node (102).
    nodes.push_back({ 200, kLat + 2000, kLon + 1000, {} });
    nodes.push_back({ 201, kLat + 2000, kLon + 2000, {} });

    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101, 102, 103, 104 }, { { 1, 2 } } },
        { 600, { 102, 200, 201 }, { { 1, 2 } } },
    };

    const auto path = writePbf("map_build_split.pbf", makeFile(nodes, ways, strings));
    const auto result = run(path);
    check(result.ok, "a crossing file extracts");
    if (!result.ok)
    {
        return;
    }

    std::size_t throughSegments = 0;
    std::size_t sideSegments = 0;
    for (const auto& segment : result.segments)
    {
        if (segment.osmWayId == 500)
        {
            ++throughSegments;
        }
        if (segment.osmWayId == 600)
        {
            ++sideSegments;
        }
    }

    check(throughSegments == 2, "the through road splits into two at the junction");
    check(sideSegments == 1, "and the side road, which meets it at its own end, stays whole");

    // The pieces must meet at the junction node, or the router cannot turn.
    bool joined = false;
    for (const auto& a : result.segments)
    {
        if (a.osmWayId != 500)
        {
            continue;
        }
        for (const auto& b : result.segments)
        {
            if (b.osmWayId == 600 && (a.toNodeId == b.fromNodeId || a.fromNodeId == b.fromNodeId))
            {
                joined = true;
            }
        }
    }
    check(joined, "and the two roads share the junction node");

    std::filesystem::remove(path);
}

void test_a_way_does_not_split_at_a_node_only_it_uses()
{
    // The other half. A road with many shape points must NOT become a segment
    // per point: that is a graph several times larger with a degree-two node
    // between every pair, which costs memory and slows every expansion.
    const std::vector<std::string> strings { "", "highway", "primary" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 20; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 300, kLon + i * 100, {} });
    }

    std::vector<osm_test::WaySpec> way { { 500, {}, { { 1, 2 } } } };
    for (int i = 0; i < 20; ++i)
    {
        way[0].refs.push_back(100 + i);
    }

    const auto path = writePbf("map_build_shape.pbf", makeFile(nodes, way, strings));
    const auto result = run(path);
    check(result.ok, "a twenty-point road extracts");
    if (!result.ok)
    {
        return;
    }

    check(result.segments.size() == 1, "and stays ONE segment despite twenty points");
    if (result.segments.size() == 1)
    {
        check(result.segments[0].geometry.size() == 40, "keeping every shape point");
    }

    std::filesystem::remove(path);
}

void test_an_unroutable_way_produces_no_segments()
{
    // A river is drawn and is not routable, so the graph must not contain it.
    const std::vector<std::string> strings { "", "waterway", "river", "highway", "residential" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 6; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }

    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101, 102 }, { { 1, 2 } } },  // waterway=river
        { 600, { 103, 104, 105 }, { { 3, 4 } } },  // highway=residential
    };

    const auto path = writePbf("map_build_river.pbf", makeFile(nodes, ways, strings));
    const auto result = run(path);
    check(result.ok, "a river and a road extract");
    if (!result.ok)
    {
        return;
    }

    check(result.stats.drawnWays == 2, "both are drawn");
    check(result.stats.routableWays == 1, "and only one is routable");
    check(result.segments.size() == 1, "so only one produces a segment");
    if (result.segments.size() == 1)
    {
        check(result.segments[0].osmWayId == 600, "and it is the road, not the river");
    }

    std::filesystem::remove(path);
}

void test_a_way_with_an_unresolvable_vertex_is_dropped_whole()
{
    // A way referencing a node the file does not contain. Dropped ENTIRELY --
    // not truncated to the vertices that did resolve, which would leave a road
    // that is drawn and quietly unusable.
    const std::vector<std::string> strings { "", "highway", "residential" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 3; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }

    std::vector<osm_test::WaySpec> ways {
        // Node 999 is not in the file.
        { 500, { 100, 101, 999 }, { { 1, 2 } } },
        { 600, { 100, 101, 102 }, { { 1, 2 } } },
    };

    const auto path = writePbf("map_build_dangling.pbf", makeFile(nodes, ways, strings));
    const auto result = run(path);
    check(result.ok, "a file with a dangling reference still extracts");
    if (!result.ok)
    {
        return;
    }

    for (const auto& segment : result.segments)
    {
        check(segment.osmWayId != 500,
              "the way with the missing vertex contributes NOTHING -- not even its good part");
    }

    check(result.stats.droppedAtBoundary + result.stats.droppedInInterior == 1,
          "and the drop is counted");

    std::filesystem::remove(path);
}

void test_counting_does_not_need_a_draw_sink()
{
    const std::vector<std::string> strings { "", "highway", "motorway" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 4; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }
    std::vector<osm_test::WaySpec> ways { { 500, { 100, 101, 102, 103 }, { { 1, 2 } } } };

    const auto path = writePbf("map_build_stats.pbf", makeFile(nodes, ways, strings));

    map_build::ExtractOptions options;
    options.input = path;
    options.progressEvery = 0;

    // No draw sink and no restriction sink. Counting and classification are
    // properties of the extractor and not of who is listening, so a caller that
    // wants only the numbers should get them without having to ask for a
    // different kind of run.
    std::size_t emitted = 0;
    auto stats = map_build::extract(
        options, [&emitted](road_graph::Builder::SegmentInput&&) { ++emitted; });

    check(stats.has_value(), "a run with no draw sink succeeds");
    if (stats)
    {
        check(emitted == 1, "the segment sink still fires");
        check(stats->routableWays == 1, "the road is counted");
        check(stats->renderClasses.count("motorway") == 1, "and classified");
        check(stats->labels.empty(), "and nothing was drawn or labelled");
    }

    std::filesystem::remove(path);
}

void test_classification_reaches_the_segment()
{
    // The join between map_rules and road_graph. If this breaks, the graph is
    // built from default-constructed classifications: everything unroutable,
    // every speed zero.
    const std::vector<std::string> strings { "", "highway", "motorway", "maxspeed", "65 mph",
                                             "oneway", "yes" };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 3; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }
    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101, 102 }, { { 1, 2 }, { 3, 4 }, { 5, 6 } } },
    };

    const auto path = writePbf("map_build_class.pbf", makeFile(nodes, ways, strings));
    const auto result = run(path);
    check(result.ok, "a tagged motorway extracts");
    if (!result.ok || result.segments.empty())
    {
        return;
    }

    const auto& classification = result.segments[0].classification;
    check(classification.routeClass == map_rules::RouteClass::Motorway, "classified as a motorway");
    check(classification.hasPosted, "with its posted limit");
    check(classification.postedSpeedKph == 105, "converted from mph");
    check(classification.onewayForward, "and its direction");

    std::filesystem::remove(path);
}

void test_a_turn_restriction_relation_is_extracted()
{
    // A T junction with a no_left_turn. What this pins is that a restriction
    // survives the trip from a relation's way ids to the builder -- if it did
    // not, routing would silently permit a turn that is signed against.
    const std::vector<std::string> strings {
        "",          // 0
        "highway",   // 1
        "residential", // 2
        "type",      // 3
        "restriction", // 4
        "no_left_turn", // 5
        "from",      // 6
        "via",       // 7
        "to",        // 8
    };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 5; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon + i * 500, {} });
    }

    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101, 102 }, { { 1, 2 } } },
        { 600, { 102, 103, 104 }, { { 1, 2 } } },
    };

    osm_test::RelationSpec restriction;
    restriction.id = 9000;
    restriction.tags = { { 3, 4 }, { 4, 5 } };  // type=restriction, restriction=no_left_turn
    restriction.members = {
        { 500, 1, 6 },  // way 500, role "from"
        { 102, 0, 7 },  // node 102, role "via"
        { 600, 1, 8 },  // way 600, role "to"
    };

    const auto path =
        writePbf("map_build_restriction.pbf", makeFile(nodes, ways, strings, { restriction }));
    const auto result = run(path);
    check(result.ok, "a file with a restriction extracts");
    if (!result.ok)
    {
        return;
    }

    check(result.stats.restrictionsSeen == 1, "the relation is seen");
    check(result.stats.restrictionsViaNode == 1, "and recognised as a via-node restriction");
    check(result.restrictions.size() == 1, "and reaches the builder");
    if (result.restrictions.size() == 1)
    {
        check(result.restrictions[0].fromWayId == 500, "with the from way");
        check(result.restrictions[0].viaNodeId == 102, "the via node");
        check(result.restrictions[0].toWayId == 600, "the to way");
        check(!result.restrictions[0].only, "and the right sense");
    }

    std::filesystem::remove(path);
}

void test_a_via_way_restriction_is_counted_rather_than_guessed()
{
    // A via-WAY restriction spans a path rather than a junction. Picking a
    // nearby node instead would ban a turn somewhere else on the same road --
    // a route that silently avoids a legal manoeuvre, which nobody would ever
    // trace back to this.
    const std::vector<std::string> strings {
        "", "highway", "residential", "type", "restriction", "no_left_turn", "from", "via", "to",
    };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 5; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }
    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101 }, { { 1, 2 } } },
        { 550, { 101, 102 }, { { 1, 2 } } },
        { 600, { 102, 103 }, { { 1, 2 } } },
    };

    osm_test::RelationSpec restriction;
    restriction.id = 9000;
    restriction.tags = { { 3, 4 }, { 4, 5 } };
    restriction.members = {
        { 500, 1, 6 },
        { 550, 1, 7 },  // via is a WAY
        { 600, 1, 8 },
    };

    const auto path =
        writePbf("map_build_via_way.pbf", makeFile(nodes, ways, strings, { restriction }));
    const auto result = run(path);
    check(result.ok, "a file with a via-way restriction extracts");
    if (!result.ok)
    {
        return;
    }

    check(result.stats.restrictionsSeen == 1, "the relation is seen");
    check(result.stats.restrictionsViaWay == 1, "and counted as via-way");
    check(result.restrictions.empty(), "but NOT guessed at");

    std::filesystem::remove(path);
}

void test_an_only_restriction_keeps_its_sense()
{
    const std::vector<std::string> strings {
        "", "highway", "residential", "type", "restriction", "only_straight_on", "from", "via", "to",
    };

    std::vector<osm_test::DenseNodeSpec> nodes;
    for (int i = 0; i < 5; ++i)
    {
        nodes.push_back({ 100 + i, kLat + i * 1000, kLon, {} });
    }
    std::vector<osm_test::WaySpec> ways {
        { 500, { 100, 101, 102 }, { { 1, 2 } } },
        { 600, { 102, 103, 104 }, { { 1, 2 } } },
    };

    osm_test::RelationSpec restriction;
    restriction.id = 9000;
    restriction.tags = { { 3, 4 }, { 4, 5 } };
    restriction.members = { { 500, 1, 6 }, { 102, 0, 7 }, { 600, 1, 8 } };

    const auto path =
        writePbf("map_build_only.pbf", makeFile(nodes, ways, strings, { restriction }));
    const auto result = run(path);
    check(result.ok, "a file with an only_ restriction extracts");
    if (result.ok && result.restrictions.size() == 1)
    {
        check(result.restrictions[0].only,
              "and it keeps the ONLY sense, which bans every other turn rather than one");
    }

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_single_road_becomes_one_segment();
    test_a_way_splits_where_another_way_touches_it();
    test_a_way_does_not_split_at_a_node_only_it_uses();
    test_an_unroutable_way_produces_no_segments();
    test_a_way_with_an_unresolvable_vertex_is_dropped_whole();
    test_counting_does_not_need_a_draw_sink();
    test_classification_reaches_the_segment();
    test_a_turn_restriction_relation_is_extracted();
    test_a_via_way_restriction_is_counted_rather_than_guessed();
    test_an_only_restriction_keeps_its_sense();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all extraction checks passed");
    return 0;
}
