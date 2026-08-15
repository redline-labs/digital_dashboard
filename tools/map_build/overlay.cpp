// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>
#include <string>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/verbs.h"
#include "road_graph/contraction.h"
#include "road_graph/graph.h"

namespace map_build
{

void addOverlayOptions(cxxopts::Options& options)
{
    options.add_options()("g,graph", "Graph file to contract.", cxxopts::value<std::string>())(
        "o,output", "Overlay file to write. Defaults to <graph>.overlay.",
        cxxopts::value<std::string>())(
        "witness-settle", "Nodes a witness search may settle before giving up.",
        cxxopts::value<std::uint64_t>()->default_value("200"))(
        "witness-hops", "Hops a witness search may take before giving up.",
        cxxopts::value<std::uint64_t>()->default_value("5"))(
        "stop-at", "Fraction of nodes to contract; the rest are left as a searchable core.",
        cxxopts::value<double>()->default_value("0.95"))(
        "quiet", "No progress lines.", cxxopts::value<bool>()->default_value("false"));
}

int runOverlay(cli::Context& context)
{
    auto path = context.requireString("graph");
    if (!path)
    {
        return cli::kUsage;
    }

    auto graph = road_graph::Graph::open(*path);
    if (!graph)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(graph.error()));
        return cli::kFailure;
    }

    std::filesystem::path out = context.stringOr("output", "");
    if (out.empty())
    {
        out = std::filesystem::path(*path);
        out += ".overlay";
    }

    road_graph::ContractionOptions options;
    options.witnessSettleLimit = static_cast<std::size_t>(context.uintOr("witness-settle", 200));
    options.witnessHopLimit = static_cast<std::size_t>(context.uintOr("witness-hops", 5));
    options.stopAtFraction = context.doubleOr("stop-at", 0.95);
    if (context.flag("quiet"))
    {
        options.progressEvery = 0;
    }

    auto stats = road_graph::buildOverlay(*graph, out, options);
    if (!stats)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(stats.error()));
        return cli::kFailure;
    }

    cli::out("built in     {:.1f} s\n", stats->buildSeconds);
    cli::out("output       {}\n", out.string());
    cli::out("expanded     {} nodes (one per directed edge)\n", stats->expandedNodes);
    cli::out("transitions  {}\n", stats->originalArcs);
    cli::out("shortcuts    {}\n", stats->shortcuts);
    cli::out("core         {} nodes left uncontracted\n", stats->coreNodes);
    cli::out("witness      {} searches\n", stats->witnessSearches);
    cli::out("size         {} MB\n", stats->bytes / (1024 * 1024));

    return cli::kOk;
}

} // namespace map_build
