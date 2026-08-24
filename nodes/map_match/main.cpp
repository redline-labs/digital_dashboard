// SPDX-License-Identifier: GPL-3.0-or-later
//
// map_match: which road the vehicle is on, and what is ahead of it.
//
// Subscribes the fused GNSS epoch, matches it onto the road graph, and
// publishes an electronic horizon. Runs on the vehicle, entirely offline.
//
// It opens the graph DIRECTLY rather than asking map_server. A matcher needs a
// candidate set and several bounded searches per fix -- dozens of lookups at
// 10 Hz -- and round-tripping those over zenoh would buy latency and a hard
// cross-process dependency for what is a pointer dereference into an mmap. A
// read-only mmap is shareable, so both processes hold the same file with no
// coordination at all.

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include <cxxopts.hpp>

#include "pub_sub/node_identity.h"
#include "road_graph/graph.h"

#include "node_config.h"
#include "services.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

// Open the graph and say what is in it, without touching the bus. The fastest
// way to find out whether a path is right -- the same idea as map_server's
// --check, which docs/map.md calls exactly that.
int runCheck(const map_match::NodeConfig& config)
{
    auto graph = road_graph::Graph::open(config.graphPath);
    if (!graph)
    {
        SPDLOG_ERROR("{}: {}", config.graphPath, road_graph::to_string(graph.error()));
        return 1;
    }

    const road_graph::FileHeader& header = graph->header();
    SPDLOG_INFO("{}", config.graphPath);
    SPDLOG_INFO("  {} segments, {} junctions, {} edges", header.segmentCount, header.nodeCount,
                header.edgeCount);
    SPDLOG_INFO("  coverage {:.5f},{:.5f} .. {:.5f},{:.5f}", header.west * 1e-7,
                header.south * 1e-7, header.east * 1e-7, header.north * 1e-7);
    SPDLOG_INFO("  built at {}", header.builtAtUnixS);
    SPDLOG_INFO("subscribing '{}', '{}', '{}' -- joined on the GSOF transmission number",
                config.position.positionKey, config.position.velocityKey, config.position.sigmaKey);
    SPDLOG_INFO("publishing '{}'", config.services.horizonKey);

    // Probe: prove a query actually comes back, because a graph that opens and
    // answers nothing is otherwise a clean bill of health.
    // Widened before adding. Two longitudes near -118 degrees are about
    // -1.18e9 each in 1e-7 degrees, and their sum overflows int32 -- which is
    // undefined behaviour that in practice yields a centre in the wrong
    // hemisphere, so the probe below reports "no road" for a graph that is
    // perfectly fine.
    const auto centreLat = static_cast<road_graph::Coord>(
        (static_cast<std::int64_t>(header.south) + header.north) / 2);
    const auto centreLon = static_cast<road_graph::Coord>(
        (static_cast<std::int64_t>(header.west) + header.east) / 2);
    const auto matches = graph->nearest(centreLat, centreLon, 5000.0, 1);
    if (matches.empty())
    {
        SPDLOG_ERROR("  no road within 5 km of the centre of the coverage");
        return 1;
    }
    SPDLOG_INFO("  probe: nearest road to the centre is {:.0f} m away", matches[0].distanceM);

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::string configPath;
    bool check = false;
    bool debug = false;

    try
    {
        cxxopts::Options options("map_match", "Match GNSS onto the road graph");
        options.add_options()("c,config", "YAML configuration file.",
                              cxxopts::value<std::string>(configPath))(
            "check", "Open the graph, report what is in it, and exit.",
            cxxopts::value<bool>(check))("debug", "Verbose logging.",
                                         cxxopts::value<bool>(debug))("h,help", "Print usage.");

        const auto args = options.parse(argc, argv);
        if (args.count("help") != 0)
        {
            SPDLOG_INFO("{}", options.help());
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        return 2;
    }

    if (debug)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    if (configPath.empty())
    {
        SPDLOG_ERROR("--config is required");
        return 2;
    }

    map_match::NodeConfig config;
    if (!map_match::load_node_config(configPath, config))
    {
        return 1;
    }

    if (check)
    {
        return runCheck(config);
    }

    auto graph = road_graph::Graph::open(config.graphPath);
    if (!graph)
    {
        // Fatal here, unlike map_server's archives: a matcher with no graph has
        // nothing to say at all, and a node that publishes an empty horizon
        // forever is worse than one that does not start.
        SPDLOG_ERROR("{}: {}", config.graphPath, road_graph::to_string(graph.error()));
        return 1;
    }

    SPDLOG_INFO("[node] {} segments, {} junctions", graph->header().segmentCount,
                graph->header().nodeCount);

    // Before any publisher, so a tool watching the bus sees the node appear
    // before the things it offers.
    pub_sub::NodeIdentity identity("map_match");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    map_match::Services services(config, *graph);

    const auto statusInterval = std::chrono::milliseconds(config.services.statusIntervalMs);
    auto nextStatus = std::chrono::steady_clock::now();

    while (gRunning.load())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus)
        {
            services.publishStatus();
            nextStatus = now + statusInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
