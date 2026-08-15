// SPDX-License-Identifier: GPL-3.0-or-later
//
// Routing, and the rules that make a route one a driver can actually follow.
//
// A router that ignores oneway, turn restrictions and U-turns still returns a
// route -- a shorter one, in fact -- and every one of those routes is wrong in a
// way that only shows up behind the wheel. So each is tested by building a graph
// where the honest answer is the LONGER way round, and requiring the router to
// take it.

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "road_graph/builder.h"
#include "road_graph/graph.h"
#include "road_graph/search.h"

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
// About 11 m.
constexpr road_graph::Coord kStep = 1000;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

map_rules::RoadClassification roadOf()
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Minor;
    out.routeClass = map_rules::RouteClass::Minor;
    out.access = map_rules::kAccessMotorcar;
    out.freeFlowSpeedKph = 50;
    return out;
}

// A straight segment between two grid points, ten steps apart.
road_graph::Builder::SegmentInput link(std::int64_t wayId, std::int64_t fromNode,
                                       std::int64_t toNode, int fromX, int fromY, int toX, int toY,
                                       const std::string& name)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, 0);
    input.osmWayId = wayId;
    input.fromNodeId = fromNode;
    input.toNodeId = toNode;
    for (int i = 0; i <= 4; ++i)
    {
        const double t = i / 4.0;
        input.geometry.push_back(
            static_cast<road_graph::Coord>(kLat + (fromY + (toY - fromY) * t) * 10 * kStep));
        input.geometry.push_back(
            static_cast<road_graph::Coord>(kLon + (fromX + (toX - fromX) * t) * 10 * kStep));
    }
    input.classification = roadOf();
    input.name = name;
    return input;
}

// Node ids on a small grid: (x, y) -> id.
std::int64_t nodeId(int x, int y)
{
    return 1000 + y * 10 + x;
}

road_graph::NodeIndex findNode(const road_graph::Graph& graph, int x, int y)
{
    const auto lat = static_cast<road_graph::Coord>(kLat + y * 10 * kStep);
    const auto lon = static_cast<road_graph::Coord>(kLon + x * 10 * kStep);
    for (std::uint32_t i = 0; i < graph.header().nodeCount; ++i)
    {
        if (std::abs(graph.nodes()[i].lat - lat) < 50 && std::abs(graph.nodes()[i].lon - lon) < 50)
        {
            return i;
        }
    }
    return road_graph::kNoNode;
}

std::vector<std::string> namesOf(const road_graph::Graph& graph, const road_graph::Route& route)
{
    std::vector<std::string> out;
    for (const road_graph::SegmentIndex index : route.segments)
    {
        out.emplace_back(graph.nameOf(graph.segments()[index]));
    }
    return out;
}

bool uses(const road_graph::Graph& graph, const road_graph::Route& route, const std::string& name)
{
    for (const std::string& used : namesOf(graph, route))
    {
        if (used == name)
        {
            return true;
        }
    }
    return false;
}

void test_a_route_is_found_and_its_geometry_runs_the_right_way()
{
    // A -> B -> C in a line. The middle segment is stored pointing the other
    // way, so this also pins that a segment traversed against its geometry is
    // REVERSED at the point of use rather than stored twice (decision 4).
    const auto path = scratch("rg_route_basic.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "First"));
    // Stored from C back to B: the router will traverse it backwards.
    builder.add(link(2, nodeId(2, 0), nodeId(1, 0), 2, 0, 1, 0, "Second"));
    check(builder.write(path, 0).has_value(), "a two-segment road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    const auto from = findNode(*graph, 0, 0);
    const auto to = findNode(*graph, 2, 0);
    check(from != road_graph::kNoNode && to != road_graph::kNoNode, "both endpoints are junctions");

    auto route = road_graph::findRoute(*graph, from, to);
    check(route.has_value(), "a route is found");
    if (!route)
    {
        return;
    }

    check(route->segments.size() == 2, "using both segments");
    check(route->distanceM > 100.0, "with a plausible distance");
    check(route->durationS > 0.0, "and a duration");

    // The geometry must run start-to-finish in TRAVEL order. If the reversed
    // segment were emitted in its stored direction the polyline would double
    // back on itself -- which draws as a road that goes there and comes back.
    const auto& g = route->geometry;
    check(g.size() >= 4, "the geometry has points");
    if (g.size() >= 4)
    {
        const road_graph::Coord firstLon = g[1];
        const road_graph::Coord lastLon = g[g.size() - 1];
        check(lastLon > firstLon, "and runs west to east, in travel order");

        bool monotonic = true;
        for (std::size_t i = 3; i < g.size(); i += 2)
        {
            if (g[i] < g[i - 2] - 10)
            {
                monotonic = false;
            }
        }
        check(monotonic, "without doubling back");
    }

    std::filesystem::remove(path);
}

void test_a_oneway_forces_the_long_way_round()
{
    // Direct link A->B exists but is one-way the wrong way. The router must go
    // round three sides of the square rather than drive up it.
    const auto path = scratch("rg_route_oneway.graph");

    road_graph::Builder builder;

    auto direct = link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "Direct");
    // One-way from B to A, i.e. against the direction we want.
    direct.classification.onewayBackward = true;
    builder.add(std::move(direct));

    builder.add(link(2, nodeId(0, 0), nodeId(0, 1), 0, 0, 0, 1, "Up"));
    builder.add(link(3, nodeId(0, 1), nodeId(1, 1), 0, 1, 1, 1, "Across"));
    builder.add(link(4, nodeId(1, 1), nodeId(1, 0), 1, 1, 1, 0, "Down"));

    check(builder.write(path, 0).has_value(), "the block writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    auto route = road_graph::findRoute(*graph, findNode(*graph, 0, 0), findNode(*graph, 1, 0));
    check(route.has_value(), "a route is found");
    if (!route)
    {
        return;
    }

    check(!uses(*graph, *route, "Direct"),
          "and does NOT drive the wrong way up the one-way street");
    check(route->segments.size() == 3, "taking the three-sided way round instead");

    std::filesystem::remove(path);
}

void test_a_turn_restriction_forces_a_detour()
{
    // THE case. A T junction where the left turn is banned. Both roads exist,
    // both are two-way, and the direct route is shorter -- a router that
    // ignored the restriction would return it, and the driver would find they
    // cannot make the turn.
    const auto path = scratch("rg_route_restriction.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(1, 0), nodeId(1, 1), 1, 0, 1, 1, "Approach"));
    builder.add(link(2, nodeId(1, 1), nodeId(0, 1), 1, 1, 0, 1, "Banned"));
    // The long way: continue north, then west, then back south.
    builder.add(link(3, nodeId(1, 1), nodeId(1, 2), 1, 1, 1, 2, "North"));
    builder.add(link(4, nodeId(1, 2), nodeId(0, 2), 1, 2, 0, 2, "West"));
    builder.add(link(5, nodeId(0, 2), nodeId(0, 1), 0, 2, 0, 1, "South"));

    road_graph::Builder::RestrictionInput restriction;
    restriction.fromWayId = 1;
    restriction.viaNodeId = nodeId(1, 1);
    restriction.toWayId = 2;
    restriction.only = false;
    builder.addRestriction(restriction);

    check(builder.write(path, 0).has_value(), "the junction writes");
    check(builder.restrictionCounts().resolved == 1,
          "and the restriction resolves against the way index");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    check(graph->turnRestrictions().size() == 1, "the graph carries the restriction");

    auto route = road_graph::findRoute(*graph, findNode(*graph, 1, 0), findNode(*graph, 0, 1));
    check(route.has_value(), "a route is still found");
    if (!route)
    {
        return;
    }

    check(!uses(*graph, *route, "Banned"), "and it does NOT make the banned turn");
    check(route->segments.size() == 4, "going the long way round instead");

    std::filesystem::remove(path);
}

void test_the_same_junction_without_the_restriction_takes_the_short_way()
{
    // The control. Without the restriction the direct route is the answer --
    // which is what makes the previous test a statement about the restriction
    // rather than about the geometry.
    const auto path = scratch("rg_route_control.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(1, 0), nodeId(1, 1), 1, 0, 1, 1, "Approach"));
    builder.add(link(2, nodeId(1, 1), nodeId(0, 1), 1, 1, 0, 1, "Banned"));
    builder.add(link(3, nodeId(1, 1), nodeId(1, 2), 1, 1, 1, 2, "North"));
    builder.add(link(4, nodeId(1, 2), nodeId(0, 2), 1, 2, 0, 2, "West"));
    builder.add(link(5, nodeId(0, 2), nodeId(0, 1), 0, 2, 0, 1, "South"));

    check(builder.write(path, 0).has_value(), "the junction writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    auto route = road_graph::findRoute(*graph, findNode(*graph, 1, 0), findNode(*graph, 0, 1));
    check(route.has_value(), "a route is found");
    if (route)
    {
        check(uses(*graph, *route, "Banned"),
              "and with no restriction it DOES take the short way");
        check(route->segments.size() == 2, "using two segments");
    }

    std::filesystem::remove(path);
}

void test_an_only_restriction_bans_every_other_turn()
{
    // only_straight_on has the opposite sense from a prohibition: it permits
    // one turn and bans the rest. A reader that treated it as a prohibition
    // would ban the one legal manoeuvre and permit the illegal ones.
    const auto path = scratch("rg_route_only.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(1, 0), nodeId(1, 1), 1, 0, 1, 1, "Approach"));
    builder.add(link(2, nodeId(1, 1), nodeId(1, 2), 1, 1, 1, 2, "Straight"));
    builder.add(link(3, nodeId(1, 1), nodeId(0, 1), 1, 1, 0, 1, "Left"));
    // A way round to the left turn's far end, so a route there is still
    // possible without the banned turn.
    builder.add(link(4, nodeId(1, 2), nodeId(0, 2), 1, 2, 0, 2, "Detour"));
    builder.add(link(5, nodeId(0, 2), nodeId(0, 1), 0, 2, 0, 1, "Back"));

    road_graph::Builder::RestrictionInput only;
    only.fromWayId = 1;
    only.viaNodeId = nodeId(1, 1);
    only.toWayId = 2;  // only straight on
    only.only = true;
    builder.addRestriction(only);

    check(builder.write(path, 0).has_value(), "the junction writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    auto route = road_graph::findRoute(*graph, findNode(*graph, 1, 0), findNode(*graph, 0, 1));
    check(route.has_value(), "a route is found");
    if (route)
    {
        check(!uses(*graph, *route, "Left"),
              "and the only_ restriction banned the turn it did not name");
        check(uses(*graph, *route, "Straight"), "forcing the permitted one");
    }

    std::filesystem::remove(path);
}

void test_a_u_turn_is_refused_where_there_is_a_choice()
{
    // A router that allows U-turns finds shorter routes that no driver can
    // follow. It must still allow one at a dead end, or a cul-de-sac becomes
    // unreachable.
    const auto path = scratch("rg_route_uturn.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "Main"));
    builder.add(link(2, nodeId(1, 0), nodeId(2, 0), 1, 0, 2, 0, "Cul de sac"));

    check(builder.write(path, 0).has_value(), "the road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // Out to the dead end and back: the only way home is a U-turn, and it has
    // to be allowed there.
    auto out = road_graph::findRoute(*graph, findNode(*graph, 0, 0), findNode(*graph, 2, 0));
    check(out.has_value(), "a route to the dead end exists");

    auto back = road_graph::findRoute(*graph, findNode(*graph, 2, 0), findNode(*graph, 0, 0));
    check(back.has_value(), "and a route back, which needs a U-turn at the end");

    std::filesystem::remove(path);
}

void test_no_route_between_disconnected_pieces()
{
    const auto path = scratch("rg_route_split.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "Island A"));
    builder.add(link(2, nodeId(5, 5), nodeId(6, 5), 5, 5, 6, 5, "Island B"));
    check(builder.write(path, 0).has_value(), "two disconnected roads write");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    auto route = road_graph::findRoute(*graph, findNode(*graph, 0, 0), findNode(*graph, 6, 5));
    check(!route.has_value(), "no route is found between disconnected pieces");

    std::filesystem::remove(path);
}

void test_bounded_distance_gives_up_rather_than_expanding()
{
    // What the matcher leans on. Two candidates that do not connect nearby must
    // come back as "not close" quickly, not after expanding the city.
    const auto path = scratch("rg_bounded.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "A"));
    builder.add(link(2, nodeId(1, 0), nodeId(2, 0), 1, 0, 2, 0, "B"));
    check(builder.write(path, 0).has_value(), "the road writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    const auto from = findNode(*graph, 0, 0);
    const auto to = findNode(*graph, 2, 0);

    auto near = road_graph::boundedDistance(*graph, from, to, 10000.0);
    check(near.has_value(), "a reachable pair inside the limit is found");
    if (near)
    {
        check(*near > 100.0, "with a real distance");
    }

    auto far = road_graph::boundedDistance(*graph, from, to, 50.0);
    check(!far.has_value(), "and the same pair past the limit gives up");

    auto same = road_graph::boundedDistance(*graph, from, from, 10.0);
    check(same.has_value() && *same == 0.0, "a node reaches itself at zero cost");

    std::filesystem::remove(path);
}

void test_a_graph_with_no_restrictions_allows_everything()
{
    // The absent-section path. A graph built before restrictions existed, or
    // from an extract with none, must not have every turn banned.
    const auto path = scratch("rg_no_restrictions.graph");

    road_graph::Builder builder;
    builder.add(link(1, nodeId(0, 0), nodeId(1, 0), 0, 0, 1, 0, "A"));
    builder.add(link(2, nodeId(1, 0), nodeId(2, 0), 1, 0, 2, 0, "B"));
    check(builder.write(path, 0).has_value(), "the graph writes");

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    check(graph->turnRestrictions().empty(), "it carries no restrictions");
    check(graph->turnAllowed(0, 0, 1), "and every turn is allowed");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_route_is_found_and_its_geometry_runs_the_right_way();
    test_a_oneway_forces_the_long_way_round();
    test_a_turn_restriction_forces_a_detour();
    test_the_same_junction_without_the_restriction_takes_the_short_way();
    test_an_only_restriction_bans_every_other_turn();
    test_a_u_turn_is_refused_where_there_is_a_choice();
    test_no_route_between_disconnected_pieces();
    test_bounded_distance_gives_up_rather_than_expanding();
    test_a_graph_with_no_restrictions_allows_everything();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all routing checks passed");
    return 0;
}
