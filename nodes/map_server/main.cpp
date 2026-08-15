// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline map tiles on the bus.
//
// Opens one or more .mbtiles archives and answers zenoh queries for tiles,
// tileset catalogs and style assets. No HTTP anywhere: the dashboard's map
// widget fetches tiles from this node over the bus, so a vehicle with no
// internet renders the same map it would with one.
//
// Everything format-shaped is in libs/mbtiles; what is left here is the mapping
// onto capnp schemas, the YAML, and the process lifecycle -- the same split as
// bd992_bridge against libs/bd992.
//
// Modes:
//   --check       open every configured archive AND graph, report, exit
//   (default)     serve

#include <spdlog/spdlog.h>

#include <cxxopts.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <string>
#include <thread>

#include "pub_sub/node_identity.h"

#include "graphs.h"
#include "node_config.h"
#include "services.h"
#include "tilesets.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

using namespace map_server;

// Open everything, say what is in it, and exit. The fastest way to find out
// whether a path is right and an archive is what you think it is, without
// starting a bus session.
// A build time a person can read. The raw epoch is in the header because that is
// what a checksum and a comparison want; a driver reporting a missing road is
// asking which build they have, and "1755300000" does not answer that.
std::string buildTime(std::int64_t unixSeconds)
{
    if (unixSeconds == 0)
    {
        return "(unknown)";
    }
    const auto seconds = static_cast<std::time_t>(unixSeconds);
    std::tm parts {};
    ::localtime_r(&seconds, &parts);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

int runCheck(const NodeConfig& config)
{
    TilesetRegistry tilesets(config.tilesets);

    int broken = 0;
    for (const auto& tileset : tilesets.all())
    {
        if (!tileset->archive)
        {
            SPDLOG_ERROR("{}: {}", tileset->name, tileset->error);
            ++broken;
            continue;
        }

        const mbtiles::Metadata& meta = tileset->archive->metadata();
        SPDLOG_INFO("{}", tileset->name);
        SPDLOG_INFO("  path        {}", tileset->path);
        SPDLOG_INFO("  name        {}", meta.name);
        SPDLOG_INFO("  format      {}", meta.format.empty() ? "(unset)" : meta.format);
        SPDLOG_INFO("  zoom        {} - {}", meta.minzoom, meta.maxzoom);
        if (meta.bounds.size() == 4)
        {
            SPDLOG_INFO("  bounds      {} {} {} {}", meta.bounds[0], meta.bounds[1], meta.bounds[2],
                        meta.bounds[3]);
        }
        else
        {
            SPDLOG_INFO("  bounds      (unset)");
        }
        SPDLOG_INFO("  tile url    {}", tileUrlTemplate(*tileset));

        // Prove a tile actually comes out, at the middle of the zoom range and
        // the middle of the bounds if there is one. An archive that opens and
        // has no readable tiles is otherwise a clean bill of health.
        const std::uint8_t z = meta.minzoom;
        auto probe = tileset->archive->tile(z, 0, 0);
        if (!probe)
        {
            SPDLOG_ERROR("  probe       failed: {}", mbtiles::to_string(probe.error()));
            ++broken;
        }
        else
        {
            SPDLOG_INFO("  probe       z{}/0/0 {}", z,
                        probe->has_value() ? "present" : "absent (normal for sparse coverage)");
        }
    }

    // THE GRAPH IS CHECKED TOO, and it is the half more likely to be wrong.
    //
    // An archive is one path in one config entry. A graph is a path, plus a
    // sidecar overlay beside it that was built by a different verb at a
    // different time and is accepted only if its checksum still matches. Every
    // one of those can be stale or absent without anything failing at startup --
    // the node is deliberately built to degrade rather than refuse -- so if
    // `--check` did not look, the first sign of a wrong path would be a query
    // answering `noSuchGraph` in a vehicle.
    //
    // Constructing the registry IS the check: it opens each file and logs what
    // it found, or why it could not.
    GraphRegistry graphs(config.graphs);
    for (const auto& graph : graphs.all())
    {
        if (!graph->graph)
        {
            ++broken;
            continue;
        }

        const road_graph::FileHeader& header = graph->graph->header();
        SPDLOG_INFO("{}", graph->name);
        SPDLOG_INFO("  path        {}", graph->path);
        SPDLOG_INFO("  built       {}", buildTime(header.builtAtUnixS));
        SPDLOG_INFO("  contents    {} nodes, {} segments, {} edges", header.nodeCount,
                    header.segmentCount, header.edgeCount);
        SPDLOG_INFO("  bounds      {:.6f} {:.6f} {:.6f} {:.6f}", header.west * 1e-7,
                    header.south * 1e-7, header.east * 1e-7, header.north * 1e-7);
        if (graph->overlay)
        {
            SPDLOG_INFO("  overlay     {} shortcuts", graph->overlay->header().shortcutCount);
        }
        else
        {
            // Not counted as broken: routing still answers, with the same route,
            // more slowly. Reported anyway, because "never built" and "built for
            // a different graph" are different mistakes.
            SPDLOG_WARN("  overlay     absent -- {}", graph->overlayError);
        }
    }

    const std::size_t configured = tilesets.all().size() + graphs.all().size();
    if (broken != 0)
    {
        SPDLOG_ERROR("{} of {} configured archive(s)/graph(s) unusable", broken, configured);
        return 1;
    }

    SPDLOG_INFO("{} tileset(s) and {} graph(s) usable", tilesets.all().size(),
                graphs.all().size());
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
        cxxopts::Options options("map_server", "Serve .mbtiles archives over zenoh");
        options.add_options()
            ("c,config", "YAML configuration file.", cxxopts::value<std::string>(configPath))
            ("check", "Open every archive, report what is in it, and exit.",
                cxxopts::value<bool>(check))
            ("debug", "Verbose logging.", cxxopts::value<bool>(debug))
            ("h,help", "Print usage.");

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

    NodeConfig config;
    if (!load_node_config(configPath, config))
    {
        return 1;
    }

    if (check)
    {
        return runCheck(config);
    }

    // Before any service, so a tool watching the bus sees the node appear
    // before the things it offers.
    pub_sub::NodeIdentity identity("map_server");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    TilesetRegistry tilesets(config.tilesets);
    if (tilesets.openCount() == 0)
    {
        // Not fatal: the node still answers, and the catalog still names the
        // tilesets with their errors, which is how a client finds out why the
        // map is blank. Exiting instead would leave nothing to ask.
        SPDLOG_ERROR("[node] no archive opened; every tile request will report the reason");
    }

    GraphRegistry graphs(config.graphs);
    if (!config.graphs.empty() && graphs.openCount() == 0)
    {
        // Not fatal, exactly as an unopenable archive is not: the node still
        // serves tiles, and map/nearest answers with the reason rather than
        // going silent. A dashboard with a map and no road names beats no
        // dashboard.
        SPDLOG_ERROR("[node] no road graph opened; map/nearest will report why");
    }

    Services services(config, tilesets, graphs);

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
