// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/extract.h"
#include "map_build/tiler.h"
#include "map_build/verbs.h"
#include "mbtiles/archive.h"
#include "osm/error.h"

namespace map_build
{

void addTileOptions(cxxopts::Options& options)
{
    options.add_options()("i,input", "OSM PBF to read.", cxxopts::value<std::string>())(
        "o,output", "mbtiles archive to write.", cxxopts::value<std::string>())(
        "name", "Tileset name, written into the metadata.",
        cxxopts::value<std::string>()->default_value("map"))(
        "min-zoom", "Lowest zoom to build.", cxxopts::value<std::uint64_t>()->default_value("0"))(
        "max-zoom", "Highest zoom to build.", cxxopts::value<std::uint64_t>()->default_value("14"))(
        "quiet", "No progress lines.", cxxopts::value<bool>()->default_value("false"));
}

int runTile(cli::Context& context)
{
    auto input = context.requireString("input");
    auto output = context.requireString("output");
    if (!input || !output)
    {
        return cli::kUsage;
    }

    ExtractOptions extractOptions;
    extractOptions.input = *input;
    extractOptions.progressEvery = context.flag("quiet") ? 0 : 2000;

    Tiler tiler;

    const auto started = std::chrono::steady_clock::now();

    // The SAME extraction the graph verb runs, with the drawn sink attached
    // instead of the routable one. One classify() call decides both, which is
    // what makes "the route goes down a road that isn't drawn" impossible
    // rather than merely unlikely.
    auto stats = extract(
        extractOptions, [](road_graph::Builder::SegmentInput&&) {}, {},
        [&tiler](DrawInput&& feature) { tiler.add(std::move(feature)); });
    if (!stats)
    {
        SPDLOG_ERROR("{}", osm::to_string(stats.error()));
        return cli::kFailure;
    }

    SPDLOG_INFO("[tile] {} drawable features", tiler.featureCount());

    TileOptions tileOptions;
    tileOptions.minZoom = static_cast<std::uint8_t>(context.uintOr("min-zoom", 0));
    tileOptions.maxZoom = static_cast<std::uint8_t>(context.uintOr("max-zoom", 14));
    tileOptions.progressEvery = context.flag("quiet") ? 0 : 5000;

    auto writer = mbtiles::Writer::create(*output);
    if (!writer)
    {
        SPDLOG_ERROR("{}", mbtiles::to_string(writer.error()));
        return cli::kFailure;
    }

    auto tiled = tiler.write(*writer, tileOptions, context.stringOr("name", "map"), stats->west,
                             stats->south, stats->east, stats->north);
    if (!tiled)
    {
        SPDLOG_ERROR("{}", mbtiles::to_string(tiled.error()));
        return cli::kFailure;
    }

    if (auto ok = writer->finish(); !ok)
    {
        SPDLOG_ERROR("{}", mbtiles::to_string(ok.error()));
        return cli::kFailure;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // Reopened through the READER rather than trusted. An archive this tool can
    // write and Archive cannot read is the one failure that would reach the
    // dashboard as a blank map, and proving otherwise costs a second.
    auto archive = mbtiles::Archive::open(*output);
    if (!archive)
    {
        SPDLOG_ERROR("wrote an archive that will not open: {}",
                     mbtiles::to_string(archive.error()));
        return cli::kFailure;
    }

    std::error_code ec;
    const auto bytes = std::filesystem::file_size(*output, ec);

    cli::out("built in {:.1f} s\n", elapsed.count() / 1000.0);
    cli::out("features   {}\n", tiled->features);
    cli::out("tiles      {}\n", tiled->tiles);
    cli::out("size       {} MB\n", bytes / (1024 * 1024));
    cli::out("dropped    {} features too small or too spread out for their zoom\n",
             tiled->droppedTooSmall);
    cli::out("\ntiles per zoom:\n");
    for (const auto& [zoom, count] : tiled->tilesPerZoom)
    {
        cli::out("  z{:<3} {}\n", zoom, count);
    }

    return cli::kOk;
}

} // namespace map_build
