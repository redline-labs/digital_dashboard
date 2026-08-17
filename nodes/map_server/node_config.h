// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map_server node's YAML configuration.
//
// Same shape as nodes/bd992_bridge and nodes/can_bridge: plain structs with
// in-class defaults, a parse that accumulates every problem rather than
// stopping at the first, and a string-taking overload so the parser is
// testable without a file.
#ifndef MAP_SERVER_NODE_CONFIG_H
#define MAP_SERVER_NODE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace map_server
{

struct TilesetConfig
{
    // What clients ask for. Not a path: a client has no business knowing where
    // the archive lives, and the name is what stays stable when it moves.
    std::string name;
    std::string path;
};

struct GraphConfig
{
    // What clients ask for, exactly as a tileset name: a client has no business
    // knowing where the file lives.
    std::string name;
    std::string path;
};

// The race-track catalogue, which lives in extra tables INSIDE an .mbtiles.
//
// So a track layer is normally configured TWICE -- once here and once as a
// tileset, both pointing at the same file. That is deliberate and not a
// redundancy to tidy away: the two halves are read by different libraries for
// different questions (mbtiles::Archive for the tiles, track_store::Store for
// the catalogue), and either can be present without the other. Two read-only
// SQLite connections on one file is fine.
struct TracksetConfig
{
    std::string name;
    std::string path;
};

struct ServiceConfig
{
    std::string tileKey { "map/tile" };
    std::string catalogKey { "map/catalog" };
    std::string assetKey { "map/asset" };
    std::string statusKey { "map/status" };

    // The road graph services. Answered from the same process as the tiles
    // because a graph is another file on the bus, and because a mmap'd graph is
    // const after open -- so unlike the archives it needs no lock at all.
    std::string nearestKey { "map/nearest" };
    std::string routeKey { "map/route" };
    std::string graphInfoKey { "map/graph" };

    // The race-track services. Separate from the tile services because they
    // answer a different question about the same file: the catalogue is what
    // tracks exist and where, the detail is one track's full-resolution
    // geometry, and neither survives tiling.
    std::string trackCatalogKey { "map/track_catalog" };
    std::string trackDetailKey { "map/track_detail" };

    std::uint32_t statusIntervalMs { 5000 };
};

struct AssetConfig
{
    // Everything a style needs that is not a tile -- the style JSON, glyph
    // ranges, sprite sheets -- lives under here. Empty disables the asset
    // service entirely, which is a legitimate configuration for a raster-only
    // deployment.
    std::string root;

    // A ceiling on one asset, so a stray large file in the asset root cannot
    // be pulled through a zenoh query in one piece. Glyph ranges are a few kB
    // and sprite sheets a few hundred; 8 MB is far above either.
    std::uint64_t maxBytes { 8U * 1024U * 1024U };
};

struct NodeConfig
{
    std::vector<TilesetConfig> tilesets;
    std::vector<GraphConfig> graphs;
    std::vector<TracksetConfig> tracksets;
    ServiceConfig services;
    AssetConfig assets;
};

// Both report every problem they find before returning false, so a config with
// three mistakes takes one run to fix rather than three.
bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace map_server

#endif // MAP_SERVER_NODE_CONFIG_H
