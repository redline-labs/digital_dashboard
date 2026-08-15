// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/extract.h"
#include "map_build/verbs.h"
#include "osm/error.h"

namespace map_build
{

void addVerifyOptions(cxxopts::Options& options)
{
    options.add_options()("i,input", "OSM PBF to read.", cxxopts::value<std::string>())(
        "quiet", "No progress lines.", cxxopts::value<bool>()->default_value("false"));
}

int runVerify(cli::Context& context)
{
    auto input = context.requireString("input");
    if (!input)
    {
        return cli::kUsage;
    }

    ExtractOptions options;
    options.input = *input;
    // EVERY SINK IS SUPPLIED, and they all discard.
    //
    // This verb does the whole build and writes none of it. That is the point:
    // the dangling-reference count, the multipolygon assembly and every label
    // rule only happen once coordinates are resolved, so a cheaper scan would
    // print zeros for exactly the things this exists to check -- and look like
    // a clean run.
    options.progressEvery = context.flag("quiet") ? 0 : 2000;

    const auto started = std::chrono::steady_clock::now();
    auto stats = extract(
        options, [](road_graph::Builder::SegmentInput&&) {},
        [](const road_graph::Builder::RestrictionInput&) {}, [](DrawInput&&) {});
    if (!stats)
    {
        SPDLOG_ERROR("{}", osm::to_string(stats.error()));
        return cli::kFailure;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    cli::out("read in {:.1f} s\n", elapsed.count() / 1000.0);
    cli::out("blocks     {}\n", stats->blocks);
    cli::out("nodes      {}\n", stats->nodes);
    cli::out("ways       {}\n", stats->ways);
    cli::out("relations  {}\n", stats->relations);
    cli::out("\n");
    cli::out("drawn ways     {}\n", stats->drawnWays);
    cli::out("routable ways  {}\n", stats->routableWays);
    cli::out("junctions      {}\n", stats->junctions);
    cli::out("\n");
    cli::out("node store  {} referenced, {} resolved, {} MB\n", stats->referencedNodes,
             stats->resolvedNodes, stats->nodeStoreBytes / (1024 * 1024));

    cli::out("\nrender classes:\n");
    for (const auto& [name, count] : stats->renderClasses)
    {
        cli::out("  {:<12} {}\n", name, count);
    }
    cli::out("\nroute classes:\n");
    for (const auto& [name, count] : stats->routeClasses)
    {
        cli::out("  {:<12} {}\n", name, count);
    }

    cli::out("\nlabel layers:\n");
    for (const auto& [name, count] : stats->labels)
    {
        cli::out("  {:<20} {}\n", name, count);
    }

    cli::out("\nmultipolygons:\n");
    cli::out("  seen          {}\n", stats->multipolygonsSeen);
    cli::out("  assembled     {}  ({} rings)\n", stats->multipolygonsAssembled,
             stats->multipolygonRings);
    // Not an error on its own: an extract cuts relations, and a shoreline that
    // leaves the box cannot close. It IS the number to look at when a lake
    // renders as a hole.
    cli::out("  unclosed      {}\n", stats->multipolygonsUnclosed);

    cli::out("\nadministrative boundaries:\n");
    cli::out("  relations     {}  ({} unrecognised level)\n", stats->boundaryRelationsSeen,
             stats->boundaryRelationsUnrecognised);
    cli::out("  ways drawn    {}\n", stats->boundaryWaysDrawn);
    cli::out("  ways missing  {}  (expected at the extract edge)\n", stats->boundaryWaysMissing);

    cli::out("\nturn restrictions:\n");
    cli::out("  seen          {}\n", stats->restrictionsSeen);
    cli::out("  via node      {}\n", stats->restrictionsViaNode);
    // Counted rather than guessed at: a via-way restriction spans a path, and
    // picking a nearby junction instead would ban a turn somewhere else on the
    // same road.
    cli::out("  via way       {}  (not applied)\n", stats->restrictionsViaWay);
    cli::out("  unrecognised  {}\n", stats->restrictionsUnrecognised);

    cli::out("\ncoverage  {:.5f},{:.5f} .. {:.5f},{:.5f}\n", stats->west * 1e-7, stats->south * 1e-7,
             stats->east * 1e-7, stats->north * 1e-7);

    // The number this verb exists for.
    //
    // Ways dropped at the edge of the extract are normal: a boundary cuts them.
    // Ways dropped in the INTERIOR are not, and reporting one number for both
    // would leave nobody able to tell. A non-zero interior count is the signal
    // that either the file is damaged or the reader is.
    cli::out("\ndropped ways  {} at the boundary (expected), {} in the interior\n",
             stats->droppedAtBoundary, stats->droppedInInterior);

    if (stats->droppedInInterior != 0)
    {
        SPDLOG_ERROR("{} ways were dropped away from the extract boundary; "
                     "the file or the reader is wrong",
                     stats->droppedInInterior);
        return cli::kFailure;
    }

    return cli::kOk;
}

} // namespace map_build
