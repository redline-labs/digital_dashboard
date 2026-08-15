// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/extract.h"
#include "map_build/verbs.h"
#include "osm/error.h"
#include "road_graph/graph.h"

namespace map_build
{

void addGraphOptions(cxxopts::Options& options)
{
    options.add_options()("i,input", "OSM PBF to read.", cxxopts::value<std::string>())(
        "o,output", "Graph file to write.", cxxopts::value<std::string>())(
        "built-at", "Unix seconds to stamp into the header. 0 uses the wall clock.",
        cxxopts::value<std::uint64_t>()->default_value("0"))(
        "quiet", "No progress lines.", cxxopts::value<bool>()->default_value("false"));
}

int runGraph(cli::Context& context)
{
    auto input = context.requireString("input");
    auto output = context.requireString("output");
    if (!input || !output)
    {
        return cli::kUsage;
    }

    ExtractOptions options;
    options.input = *input;
    options.progressEvery = context.flag("quiet") ? 0 : 2000;

    road_graph::Builder builder;

    const auto started = std::chrono::steady_clock::now();
    auto stats = extract(
        options,
        [&builder](road_graph::Builder::SegmentInput&& segment) { builder.add(std::move(segment)); },
        [&builder](const road_graph::Builder::RestrictionInput& restriction) {
            builder.addRestriction(restriction);
        });
    if (!stats)
    {
        SPDLOG_ERROR("{}", osm::to_string(stats.error()));
        return cli::kFailure;
    }

    // Passed in rather than read inside the builder, so a build is reproducible
    // and a test can pin the number.
    std::int64_t builtAt = static_cast<std::int64_t>(context.uintOr("built-at", 0));
    if (builtAt == 0)
    {
        builtAt = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    }

    SPDLOG_INFO("[graph] writing {} segments to {}", builder.segmentCount(), *output);
    if (auto ok = builder.write(*output, builtAt); !ok)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(ok.error()));
        return cli::kFailure;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // Reopened rather than trusted. The build is offline and takes minutes, so
    // proving the artifact loads costs nothing next to shipping one that does
    // not -- and this is the only place that check is free.
    auto graph = road_graph::Graph::open(*output);
    if (!graph)
    {
        SPDLOG_ERROR("wrote a graph that will not open: {}",
                     road_graph::to_string(graph.error()));
        return cli::kFailure;
    }

    std::error_code ec;
    const auto bytes = std::filesystem::file_size(*output, ec);

    cli::out("built in {:.1f} s\n", elapsed.count() / 1000.0);
    cli::out("segments   {}\n", graph->header().segmentCount);
    cli::out("junctions  {}\n", graph->header().nodeCount);
    cli::out("edges      {}\n", graph->header().edgeCount);
    cli::out("points     {}\n", graph->header().geometryCount);
    cli::out("size       {} MB\n", bytes / (1024 * 1024));
    cli::out("coverage   {:.5f},{:.5f} .. {:.5f},{:.5f}\n", graph->header().west * 1e-7,
             graph->header().south * 1e-7, graph->header().east * 1e-7,
             graph->header().north * 1e-7);
    cli::out("dropped    {} at the boundary, {} in the interior\n", stats->droppedAtBoundary,
             stats->droppedInInterior);

    const auto& turns = builder.restrictionCounts();
    cli::out("turns      {} relations seen, {} resolved, {} unresolved, {} via-way (skipped)\n",
             stats->restrictionsSeen, turns.resolved, turns.unresolved,
             stats->restrictionsViaWay);

    if (stats->droppedInInterior != 0)
    {
        // Written, and reported as a failure. The graph is usable but has holes
        // away from its edge, which is the one thing that must not pass quietly.
        SPDLOG_ERROR("{} ways were dropped away from the extract boundary",
                     stats->droppedInInterior);
        return cli::kFailure;
    }

    return cli::kOk;
}

} // namespace map_build
