// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include "node_config/reader.h"

#include "core/core.h"

#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

#include "pub_sub/topic_key.h"

namespace map_server
{
namespace
{

// The shared failure accumulator, plus the directory the YAML file came from for
// resolving relative paths (empty for a string).
struct Context : node_config::Context
{
    std::string base_dir;
};
using node_config::readUint;
using node_config::readString;

void parseTilesets(const YAML::Node& node, std::vector<TilesetConfig>& out, Context& context)
{
    if (!node)
    {
        context.fail("tilesets: is required -- a map server with no archives serves nothing");
        return;
    }

    if (!node.IsSequence())
    {
        context.fail("tilesets must be a sequence");
        return;
    }

    std::set<std::string> names;

    for (std::size_t i = 0; i < node.size(); ++i)
    {
        const YAML::Node& entry = node[i];
        const std::string where = "tilesets[" + std::to_string(i) + "]";

        if (!entry.IsMap())
        {
            context.fail(where + " must be a mapping with `name` and `path`");
            continue;
        }

        TilesetConfig tileset;
        readString(entry, "name", tileset.name, context, where);
        readString(entry, "path", tileset.path, context, where);
        tileset.path = core::paths::expand(tileset.path, context.base_dir);

        if (tileset.name.empty())
        {
            context.fail(where + ".name is required");
            continue;
        }
        if (tileset.path.empty())
        {
            context.fail(where + ".path is required");
            continue;
        }

        // The name lands in a URL and, through the catalog, in a style's source
        // definition. A '/' in it would silently change the shape of every tile
        // URL that names it.
        if (tileset.name.find('/') != std::string::npos)
        {
            context.fail(where + ".name ('" + tileset.name + "') must not contain '/'");
            continue;
        }

        // Two tilesets with one name is not a preference to resolve. Whichever
        // the lookup happened to find would serve every request, and the other
        // archive would simply never be read -- with nothing said about it.
        if (!names.insert(tileset.name).second)
        {
            context.fail(where + ".name ('" + tileset.name + "') is already used by an earlier "
                                                             "tileset");
            continue;
        }

        out.push_back(std::move(tileset));
    }

    if (out.empty() && context.ok)
    {
        context.fail("tilesets is empty -- a map server with no archives serves nothing");
    }
}

// Graphs, the same shape as tilesets and for the same reasons: named rather
// than pathed, names unique, no '/' in a name.
void parseGraphs(const YAML::Node& node, std::vector<GraphConfig>& out, Context& context)
{
    if (!node)
    {
        // Absent is fine. A deployment that only draws maps needs no graph, and
        // the nearest/route services then answer noSuchGraph rather than
        // refusing to start.
        return;
    }
    if (!node.IsSequence())
    {
        context.fail("graphs must be a sequence");
        return;
    }

    std::set<std::string> names;

    for (std::size_t i = 0; i < node.size(); ++i)
    {
        const YAML::Node& entry = node[i];
        const std::string where = "graphs[" + std::to_string(i) + "]";

        if (!entry.IsMap())
        {
            context.fail(where + " must be a mapping with `name` and `path`");
            continue;
        }

        GraphConfig graph;
        readString(entry, "name", graph.name, context, where);
        readString(entry, "path", graph.path, context, where);
        graph.path = core::paths::expand(graph.path, context.base_dir);

        if (graph.name.empty())
        {
            context.fail(where + ".name is required");
            continue;
        }
        if (graph.path.empty())
        {
            context.fail(where + ".path is required");
            continue;
        }
        if (graph.name.find('/') != std::string::npos)
        {
            context.fail(where + ".name ('" + graph.name + "') must not contain '/'");
            continue;
        }
        if (!names.insert(graph.name).second)
        {
            // Two graphs with one name is a server that starts cleanly and
            // answers from whichever it happened to store second.
            context.fail(where + ".name ('" + graph.name + "') is already used");
            continue;
        }

        out.push_back(std::move(graph));
    }
}

// Tracksets, the same shape as graphs and tilesets. Absent is fine: without one
// the track services answer noSuchTileset and the map is unaffected.
void parseTracksets(const YAML::Node& node, std::vector<TracksetConfig>& out, Context& context)
{
    if (!node)
    {
        return;
    }
    if (!node.IsSequence())
    {
        context.fail("tracksets must be a sequence");
        return;
    }

    std::set<std::string> names;

    for (std::size_t i = 0; i < node.size(); ++i)
    {
        const YAML::Node& entry = node[i];
        const std::string where = "tracksets[" + std::to_string(i) + "]";

        if (!entry.IsMap())
        {
            context.fail(where + " must be a mapping with `name` and `path`");
            continue;
        }

        TracksetConfig trackset;
        readString(entry, "name", trackset.name, context, where);
        readString(entry, "path", trackset.path, context, where);
        trackset.path = core::paths::expand(trackset.path, context.base_dir);

        if (trackset.name.empty())
        {
            context.fail(where + ".name is required");
            continue;
        }
        if (trackset.path.empty())
        {
            context.fail(where + ".path is required");
            continue;
        }
        if (trackset.name.find('/') != std::string::npos)
        {
            context.fail(where + ".name ('" + trackset.name + "') must not contain '/'");
            continue;
        }
        if (!names.insert(trackset.name).second)
        {
            context.fail(where + ".name ('" + trackset.name + "') is already used");
            continue;
        }

        out.push_back(std::move(trackset));
    }
}

void parseServices(const YAML::Node& node, ServiceConfig& out, Context& context)
{
    if (node)
    {
        if (!node.IsMap())
        {
            context.fail("services must be a mapping");
            return;
        }

        readString(node, "tile_key", out.tileKey, context, "services");
        readString(node, "catalog_key", out.catalogKey, context, "services");
        readString(node, "asset_key", out.assetKey, context, "services");
        readString(node, "status_key", out.statusKey, context, "services");
        readString(node, "nearest_key", out.nearestKey, context, "services");
        readString(node, "route_key", out.routeKey, context, "services");
        readString(node, "graph_info_key", out.graphInfoKey, context, "services");
        readString(node, "track_catalog_key", out.trackCatalogKey, context, "services");
        readString(node, "track_detail_key", out.trackDetailKey, context, "services");
        readUint(node, "status_interval_ms", out.statusIntervalMs, context, "services");
    }

    node_config::checkTopicKey(out.tileKey, "services.tile_key", context);
    node_config::checkTopicKey(out.catalogKey, "services.catalog_key", context);
    node_config::checkTopicKey(out.assetKey, "services.asset_key", context);
    node_config::checkTopicKey(out.statusKey, "services.status_key", context);
    node_config::checkTopicKey(out.nearestKey, "services.nearest_key", context);
    node_config::checkTopicKey(out.routeKey, "services.route_key", context);
    node_config::checkTopicKey(out.graphInfoKey, "services.graph_info_key", context);
    node_config::checkTopicKey(out.trackCatalogKey, "services.track_catalog_key", context);
    node_config::checkTopicKey(out.trackDetailKey, "services.track_detail_key", context);

    // Two services on one key both answer, and a client takes whichever reply
    // arrives first -- so a tile request would sometimes come back as a
    // catalog. That decodes against the wrong schema, which is silent: capnp
    // reads the same bytes at different offsets and hands back a plausible
    // wrong answer rather than an error.
    const std::pair<const std::string*, const char*> keys[] = {
        { &out.tileKey, "tile_key" },
        { &out.catalogKey, "catalog_key" },
        { &out.assetKey, "asset_key" },
        { &out.statusKey, "status_key" },
        { &out.nearestKey, "nearest_key" },
        { &out.routeKey, "route_key" },
        { &out.graphInfoKey, "graph_info_key" },
    };

    for (std::size_t i = 0; i < std::size(keys); ++i)
    {
        for (std::size_t j = i + 1; j < std::size(keys); ++j)
        {
            if (*keys[i].first == *keys[j].first)
            {
                context.fail(std::string("services.") + keys[i].second + " and services." +
                             keys[j].second + " are both '" + *keys[i].first + "'");
            }
        }
    }
}

void parseAssets(const YAML::Node& node, AssetConfig& out, Context& context)
{
    if (!node)
    {
        return;
    }

    if (!node.IsMap())
    {
        context.fail("assets must be a mapping");
        return;
    }

    readString(node, "root", out.root, context, "assets");
    readUint(node, "max_bytes", out.maxBytes, context, "assets");

    if (out.maxBytes == 0)
    {
        context.fail("assets.max_bytes of 0 would refuse every asset; omit it for the default");
    }
}

} // namespace

bool parse_node_config(const std::string& yaml, NodeConfig& out, const std::string& base_dir)
{
    Context context;
    context.base_dir = base_dir;

    YAML::Node root;
    try
    {
        root = YAML::Load(yaml);
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("[config] cannot parse: {}", e.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] the top level must be a mapping");
        return false;
    }

    parseTilesets(root["tilesets"], out.tilesets, context);
    parseGraphs(root["graphs"], out.graphs, context);
    parseTracksets(root["tracksets"], out.tracksets, context);
    parseServices(root["services"], out.services, context);
    parseAssets(root["assets"], out.assets, context);

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    // Read the file as text and hand it to the string-taking parser, so that
    // parser is the only implementation and the tests exercise the real one.
    std::ifstream file(path);
    if (!file)
    {
        SPDLOG_ERROR("[config] cannot open {}", path);
        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return parse_node_config(text, out, std::filesystem::path(path).parent_path().string());
}

} // namespace map_server
