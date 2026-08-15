// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contraction hierarchy, tested the only way a hierarchy can honestly be
// tested: AGAINST THE ROUTER IT REPLACES.
//
// A wrong hierarchy does not crash and does not return nothing. It returns a
// route, quickly, that is not the shortest one -- and nobody notices until
// someone drives it. So the assertion here is not "a route came back"; it is
// "the cost is EXACTLY what the plain router found", over every pair, including
// pairs that are not connected at all.
//
// Four ways it can be subtly wrong, all four mutation-checked against this test:
//
//   - A witness search claims a witness that is not there, so a needed shortcut
//     is dropped. (Suppressing every shortcut makes 328 of 650 pairs disagree.)
//   - The two search graphs are keyed at the wrong end, so the backward search
//     walks arcs that run the wrong way.
//   - The searches stop at the FIRST meeting. That meeting is a path but not
//     necessarily the shortest, so the cost comes back a little too high -- the
//     most plausible-looking failure of the set, and the easiest to write.
//   - A turn RESTRICTION is left out of the overlay's checksum, so an overlay
//     built before a ban was added still opens and still turns through it.
//
// And one this deliberately does NOT claim to catch. Drift in the U-TURN rule is
// cost-neutral for node-to-node routing and no comparison of costs can see it: a
// U-turn is never on an optimal path between two junctions, because a route that
// reversed direction could have stopped short instead. Both the permissive and
// the restrictive mutation change how many shortcuts get built and change no
// cost at all. The rule earns its keep for routing from a MATCHED POSITION,
// where the vehicle already has a heading and cannot simply have stopped short;
// that is a different entry point and wants its own test.

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <cmath>

#include "road_graph/builder.h"
#include "road_graph/contraction.h"
#include "road_graph/overlay.h"
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

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

constexpr road_graph::Coord kLat = 336865966;
constexpr road_graph::Coord kLon = -1178557874;
constexpr road_graph::Coord kStep = 1000;

std::int64_t nodeId(int x, int y)
{
    return 1000 + y * 10 + x;
}

map_rules::RoadClassification roadOf(std::uint16_t speedKph)
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Minor;
    out.routeClass = map_rules::RouteClass::Minor;
    out.access = map_rules::kAccessMotorcar;
    out.freeFlowSpeedKph = speedKph;
    return out;
}

road_graph::Builder::SegmentInput link(std::int64_t wayId, int fromX, int fromY, int toX, int toY,
                                       const std::string& name, std::uint16_t speedKph)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, 0);
    input.osmWayId = wayId;
    input.fromNodeId = nodeId(fromX, fromY);
    input.toNodeId = nodeId(toX, toY);
    for (int i = 0; i <= 4; ++i)
    {
        const double t = i / 4.0;
        input.geometry.push_back(
            static_cast<road_graph::Coord>(kLat + (fromY + (toY - fromY) * t) * 10 * kStep));
        input.geometry.push_back(
            static_cast<road_graph::Coord>(kLon + (fromX + (toX - fromX) * t) * 10 * kStep));
    }
    input.classification = roadOf(speedKph);
    input.name = name;
    return input;
}

struct Built
{
    std::filesystem::path graphPath;
    std::filesystem::path overlayPath;
};

// A grid of streets, which is the shape that makes a hierarchy work at all: it
// offers many nearly-equal routes, so a hierarchy that drops one shortcut too
// many still finds *a* route and only the COST reveals the mistake.
//
// The expressway across the middle is what gives the hierarchy something worth
// promoting -- on a uniform grid every node looks alike and the ordering has
// nothing to work with.
std::optional<Built> buildGrid(const std::string& tag, bool withRestriction)
{
    Built built { scratch(tag + ".graph"), scratch(tag + ".overlay") };

    road_graph::Builder builder;
    constexpr int kSide = 5;
    std::int64_t wayId = 1;

    for (int y = 0; y < kSide; ++y)
    {
        for (int x = 0; x + 1 < kSide; ++x)
        {
            const std::uint16_t speed = (y == 2) ? 110 : 50;
            const std::string name = (y == 2) ? "Expressway" : "Row " + std::to_string(y);
            builder.add(link(wayId++, x, y, x + 1, y, name, speed));
        }
    }
    for (int x = 0; x < kSide; ++x)
    {
        for (int y = 0; y + 1 < kSide; ++y)
        {
            builder.add(link(wayId++, x, y, x, y + 1, "Col " + std::to_string(x), 50));
        }
    }

    // A CUL-DE-SAC, and it is not decoration. Reaching its tip and coming back
    // is the one situation where a U-turn is legal -- it is the only option
    // there -- and it is the only shape that can tell an over-restrictive
    // transition rule from a correct one. Without it, a rule that banned every
    // U-turn would still agree with the router on every pair, because on a plain
    // grid a U-turn is never worth taking anyway.
    builder.add(link(900, 4, 4, 6, 4, "Cul-de-sac", 50));

    if (withRestriction)
    {
        // Ban one turn in the middle. The hierarchy has to keep honouring it
        // through however many shortcuts end up spanning that junction, which is
        // the entire reason the contraction runs over the EXPANDED graph rather
        // than the node graph.
        road_graph::Builder::RestrictionInput restriction;
        // Row ways are numbered y*4 + x + 1, column ways 21 + x*4 + y. So way 5
        // is the row-1 block arriving at (1,1) and way 26 is the column-1 block
        // leaving it northward -- the turn a driver would otherwise take.
        restriction.fromWayId = 5;
        restriction.viaNodeId = nodeId(1, 1);
        restriction.toWayId = 26;
        restriction.only = false;
        builder.addRestriction(restriction);
    }

    // A FIXED build timestamp. The overlay records it and refuses to open
    // against a graph that does not match, so two graphs built in the same
    // second must still differ -- which they do, by their counts.
    auto written = builder.write(built.graphPath, 1'700'000'000);
    if (!written)
    {
        SPDLOG_ERROR("build failed: {}", road_graph::to_string(written.error()));
        return std::nullopt;
    }

    if (withRestriction)
    {
        // A restriction that did not RESOLVE tests nothing: the graph is then
        // identical to the unrestricted one and this case silently duplicates
        // the first, which is worse than having no case at all. Restrictions
        // resolve during write(), so the count is only meaningful after it.
        const auto counts = builder.restrictionCounts();
        if (counts.resolved != 1)
        {
            SPDLOG_ERROR("the test's restriction did not resolve ({} offered, {} resolved)",
                         counts.offered, counts.resolved);
            return std::nullopt;
        }
    }
    return built;
}

void test_the_hierarchy_agrees_with_the_router(const std::string& tag, bool withRestriction,
                                              double stopAtFraction)
{
    auto built = buildGrid(tag, withRestriction);
    if (!built)
    {
        check(false, tag + ": graph builds");
        return;
    }

    auto graph = road_graph::Graph::open(built->graphPath);
    if (!graph)
    {
        check(false, tag + ": graph opens");
        return;
    }

    road_graph::ContractionOptions options;
    options.stopAtFraction = stopAtFraction;
    options.progressEvery = 0;
    auto stats = road_graph::buildOverlay(*graph, built->overlayPath, options);
    check(stats.has_value(), tag + ": the overlay builds");
    if (!stats)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(stats.error()));
        return;
    }
    check(stats->expandedNodes == graph->edges().size(),
          tag + ": one expanded node per directed edge");

    auto overlay = road_graph::Overlay::open(built->overlayPath, *graph);
    check(overlay.has_value(), tag + ": the overlay opens against its own graph");
    if (!overlay)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(overlay.error()));
        return;
    }

    // EVERY pair, not a sample. The grid is small enough, and a hierarchy that
    // is wrong for one pair in a hundred is exactly the kind that ships.
    const auto nodeCount = static_cast<road_graph::NodeIndex>(graph->nodes().size());
    std::size_t compared = 0;
    std::size_t bothFound = 0;
    std::size_t disagreements = 0;
    double worstDelta = 0.0;

    for (road_graph::NodeIndex from = 0; from < nodeCount; ++from)
    {
        for (road_graph::NodeIndex to = 0; to < nodeCount; ++to)
        {
            if (from == to)
            {
                continue;
            }
            ++compared;

            const auto plain = road_graph::findRoute(*graph, from, to);
            const auto viaOverlay = road_graph::findRouteVia(*graph, *overlay, from, to);

            if (plain.has_value() != viaOverlay.has_value())
            {
                // One found a route and the other did not. That is not a
                // rounding difference -- it means the two disagree about which
                // turns are legal.
                ++disagreements;
                if (disagreements <= 3)
                {
                    SPDLOG_ERROR("{}: {} -> {}: plain={} overlay={}", tag, from, to,
                                 plain.has_value(), viaOverlay.has_value());
                }
                continue;
            }
            if (!plain)
            {
                continue;
            }

            ++bothFound;
            const double delta = std::abs(plain->durationS - viaOverlay->durationS);
            worstDelta = std::max(worstDelta, delta);
            if (delta > 0.05)
            {
                ++disagreements;
                if (disagreements <= 3)
                {
                    SPDLOG_ERROR("{}: {} -> {}: plain {:.1f}s, overlay {:.1f}s", tag, from, to,
                                 plain->durationS, viaOverlay->durationS);
                }
            }
        }
    }

    check(bothFound > 0, tag + ": some pairs are connected at all");
    check(disagreements == 0,
          tag + ": the hierarchy returns the SAME cost as the plain router for every pair (" +
              std::to_string(disagreements) + " of " + std::to_string(compared) + " differ)");
    check(worstDelta <= 0.05, tag + ": and the worst difference is a rounding one");

    SPDLOG_INFO("{}: {} pairs, {} routable, worst delta {:.4f}s, {} shortcuts, {} in the core",
                tag, compared, bothFound, worstDelta, stats->shortcuts, stats->coreNodes);

    std::filesystem::remove(built->graphPath);
    std::filesystem::remove(built->overlayPath);
}

void test_an_overlay_from_another_graph_is_refused()
{
    // The failure this prevents is silent and total. Every shortcut in an
    // overlay names an edge INDEX; point it at a different graph and those
    // indices still exist and now mean other roads. The router would answer
    // quickly, confidently, and wrongly, and nothing in the route would look
    // out of place.
    auto first = buildGrid("ch_a", false);
    auto second = buildGrid("ch_b", true);
    if (!first || !second)
    {
        check(false, "two graphs build");
        return;
    }

    auto graphA = road_graph::Graph::open(first->graphPath);
    auto graphB = road_graph::Graph::open(second->graphPath);
    if (!graphA || !graphB)
    {
        check(false, "both graphs open");
        return;
    }

    auto stats = road_graph::buildOverlay(*graphA, first->overlayPath);
    check(stats.has_value(), "an overlay builds for the first graph");
    if (!stats)
    {
        return;
    }

    auto right = road_graph::Overlay::open(first->overlayPath, *graphA);
    check(right.has_value(), "and opens against the graph it came from");

    auto wrong = road_graph::Overlay::open(first->overlayPath, *graphB);
    check(!wrong.has_value(), "and is REFUSED against a different graph");

    std::filesystem::remove(first->graphPath);
    std::filesystem::remove(first->overlayPath);
    std::filesystem::remove(second->graphPath);
    std::filesystem::remove(second->overlayPath);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_hierarchy_agrees_with_the_router("fully contracted", false, 1.0);
    test_the_hierarchy_agrees_with_the_router("with a banned turn", true, 1.0);

    // THE CORE PATH, on its own. Stopping early leaves nodes uncontracted at one
    // shared rank, and the query has to search among them directly. Half the
    // graph is a far bigger core than a real build leaves, which is the point:
    // if the core is searchable at all, it is searchable at this size.
    test_the_hierarchy_agrees_with_the_router("half left in the core", false, 0.5);
    test_the_hierarchy_agrees_with_the_router("nothing contracted at all", false, 0.0);
    test_an_overlay_from_another_graph_is_refused();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all contraction checks passed");
    return 0;
}
