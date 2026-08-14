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

struct ServiceConfig
{
    std::string tileKey { "map/tile" };
    std::string catalogKey { "map/catalog" };
    std::string assetKey { "map/asset" };
    std::string statusKey { "map/status" };

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
    ServiceConfig services;
    AssetConfig assets;
};

// Both report every problem they find before returning false, so a config with
// three mistakes takes one run to fix rather than three.
bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace map_server

#endif // MAP_SERVER_NODE_CONFIG_H
