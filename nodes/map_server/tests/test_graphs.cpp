// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which profiles a graph offers, and which it accepts.
//
// These two answers used to come from different places: handleGraphInfo
// reported an empty list while handleRoute accepted "fastest". A client that
// discovered profiles the way map_graph.capnp says to -- ask graphInfo, then
// send one of the names it gave you -- concluded the server could not route at
// all, and there was nothing in a log to say why.
//
// So what is tested here is not the contents of the list but the AGREEMENT:
// everything reported must be accepted, and nothing else may be.

#include "graphs.h"

#include "road_graph/builder.h"

#include <spdlog/spdlog.h>

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

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

// The smallest graph that opens: one segment with two points.
bool writeTinyGraph(const std::filesystem::path& path)
{
    road_graph::Builder builder;

    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(1, 0);
    input.osmWayId = 1;
    input.fromNodeId = 1;
    input.toNodeId = 2;
    input.geometry = { kLat, kLon, kLat + 1000, kLon };
    input.classification.renderClass = map_rules::RenderClass::Minor;
    input.classification.routeClass = map_rules::RouteClass::Minor;
    input.classification.access = map_rules::kAccessMotorcar;
    input.classification.freeFlowSpeedKph = 50;
    input.name = "Main Street";
    builder.add(std::move(input));

    return builder.write(path, 0).has_value();
}

void test_a_graph_that_did_not_open_offers_nothing()
{
    // It cannot route with anything, so it must not claim it can -- and it
    // must refuse the empty "graph's default" too, which is the one a lazy
    // client sends.
    map_server::GraphEntry entry;
    entry.name = "broken";
    entry.error = "no such file";

    check(map_server::profilesOf(entry).empty(), "a graph that failed to open lists no profiles");
    check(!map_server::hasProfile(entry, ""), "and accepts no default");
    check(!map_server::hasProfile(entry, "fastest"), "and accepts nothing by name");
}

void test_everything_reported_is_accepted_and_nothing_else_is()
{
    const auto path = scratch("ms_profiles.graph");
    if (!writeTinyGraph(path))
    {
        check(false, "the test graph writes");
        return;
    }

    auto opened = road_graph::Graph::open(path);
    if (!opened)
    {
        check(false, "the test graph opens");
        std::filesystem::remove(path);
        return;
    }

    map_server::GraphEntry entry;
    entry.name = "tiny";
    entry.graph = std::make_unique<road_graph::Graph>(std::move(*opened));

    const auto profiles = map_server::profilesOf(entry);
    check(!profiles.empty(), "an open graph offers at least one profile");

    // THE invariant. Whatever the list grows to, these two must stay true.
    for (const std::string& name : profiles)
    {
        check(map_server::hasProfile(entry, name),
              "every reported profile is accepted: '" + name + "'");
    }
    check(map_server::hasProfile(entry, ""), "and the empty default is accepted");
    check(!map_server::hasProfile(entry, "avoid_tolls"),
          "while a profile that is not built is refused rather than silently defaulted");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_graph_that_did_not_open_offers_nothing();
    test_everything_reported_is_accepted_and_nothing_else_is();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map_server graph checks passed");
    return 0;
}
