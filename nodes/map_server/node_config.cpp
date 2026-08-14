// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

#include "pub_sub/topic_key.h"

namespace map_server
{
namespace
{

// Accumulates problems rather than failing on the first, so a config with
// three mistakes takes one run to fix rather than three.
struct Context
{
    bool ok { true };

    void fail(const std::string& message)
    {
        SPDLOG_ERROR("[config] {}", message);
        ok = false;
    }
};

void readString(const YAML::Node& parent, const char* key, std::string& out, Context& context,
                const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        out = parent[key].as<std::string>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + "." + key + " must be a string");
    }
}

template <typename T>
void readUint(const YAML::Node& parent, const char* key, T& out, Context& context,
              const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        const auto value = parent[key].as<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
        {
            context.fail(where + "." + key + ": " + std::to_string(value) + " is out of range");
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + "." + key + " must be a non-negative integer");
    }
}

// A zenoh key that is not in the allowed charset fails SILENTLY -- `*` and `?`
// are rejected by zenoh itself, `@` makes the segment invisible to every
// wildcard subscription, `%` is this tree's mangling separator. Checking here
// means a typo in the YAML is a startup error rather than a service nobody can
// reach. See pub_sub::topicKeyProblem().
void checkKey(const std::string& key, const char* field, Context& context)
{
    if (key.empty())
    {
        context.fail(std::string("services.") + field + " must not be empty");
        return;
    }

    const std::string problem = pub_sub::topicKeyProblem(key);
    if (!problem.empty())
    {
        context.fail(std::string("services.") + field + " ('" + key + "'): " + problem);
    }
}

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
        readUint(node, "status_interval_ms", out.statusIntervalMs, context, "services");
    }

    checkKey(out.tileKey, "tile_key", context);
    checkKey(out.catalogKey, "catalog_key", context);
    checkKey(out.assetKey, "asset_key", context);
    checkKey(out.statusKey, "status_key", context);

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

bool parse_node_config(const std::string& yaml, NodeConfig& out)
{
    Context context;

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
    return parse_node_config(text, out);
}

} // namespace map_server
