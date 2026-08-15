// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_config.h"

#include <fstream>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

#include <yaml-cpp/yaml.h>

#include "pub_sub/schema_registry.h"
#include "pub_sub/topic_key.h"

namespace map_match
{
namespace
{

// Accumulates rather than stopping, so a config with three mistakes takes one
// run to fix rather than three.
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
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    if (!node.IsScalar())
    {
        context.fail(where + "." + key + " must be a string");
        return;
    }
    out = node.as<std::string>();
}

template <typename T>
void readNumber(const YAML::Node& parent, const char* key, T& out, Context& context,
                const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    try
    {
        out = node.as<T>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + "." + key + " must be a number");
    }
}

void checkKey(const std::string& key, const char* field, Context& context)
{
    // A zenoh key outside the allowed charset fails SILENTLY -- the publisher
    // refuses and nothing subscribes. Checking here turns a typo into a startup
    // error rather than a topic nobody can reach.
    // Empty string means valid; the message is what a user can act on.
    if (const std::string problem = pub_sub::topicKeyProblem(key); !problem.empty())
    {
        context.fail(std::string(field) + " ('" + key + "'): " + problem);
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
        SPDLOG_ERROR("[config] {}", e.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] the document must be a mapping");
        return false;
    }

    readString(root, "graph", out.graphPath, context, "");
    if (out.graphPath.empty())
    {
        context.fail("graph is required: the path to a file built by tools/map_build");
    }

    if (const YAML::Node node = root["position"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("position must be a mapping");
        }
        else
        {
            readString(node, "zenoh_key", out.position.zenohKey, context, "position");
            readString(node, "schema_type", out.position.schemaType, context, "position");
            readNumber(node, "stale_after_ms", out.position.staleAfterMs, context, "position");
        }
    }

    if (const YAML::Node node = root["match"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("match must be a mapping");
        }
        else
        {
            readNumber(node, "search_radius_m", out.match.searchRadiusM, context, "match");
            readNumber(node, "beam_width", out.match.beamWidth, context, "match");
            readNumber(node, "min_sigma_m", out.match.minSigmaM, context, "match");
            readNumber(node, "max_sigma_m", out.match.maxSigmaM, context, "match");
            readNumber(node, "default_sigma_m", out.match.defaultSigmaM, context, "match");
            readNumber(node, "transition_beta_m", out.match.transitionBetaM, context, "match");
            readNumber(node, "heading_valid_above_mps", out.match.headingValidAboveMps, context,
                       "match");
            readNumber(node, "lookahead_m", out.match.lookaheadM, context, "match");
        }
    }

    if (const YAML::Node node = root["services"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("services must be a mapping");
        }
        else
        {
            readString(node, "horizon_key", out.services.horizonKey, context, "services");
            readString(node, "status_key", out.services.statusKey, context, "services");
            readNumber(node, "horizon_interval_ms", out.services.horizonIntervalMs, context,
                       "services");
            readNumber(node, "status_interval_ms", out.services.statusIntervalMs, context,
                       "services");
        }
    }

    checkKey(out.position.zenohKey, "position.zenoh_key", context);
    checkKey(out.services.horizonKey, "services.horizon_key", context);
    checkKey(out.services.statusKey, "services.status_key", context);

    // Two publishers on one key interleave two message types on one topic, and
    // a subscriber decodes whichever arrives against the schema it expected --
    // which is silent, because capnp reads the same bytes at different offsets
    // and hands back a plausible wrong answer.
    const std::pair<const std::string*, const char*> keys[] = {
        { &out.services.horizonKey, "services.horizon_key" },
        { &out.services.statusKey, "services.status_key" },
        { &out.position.zenohKey, "position.zenoh_key" },
    };
    for (std::size_t i = 0; i < std::size(keys); ++i)
    {
        for (std::size_t j = i + 1; j < std::size(keys); ++j)
        {
            if (*keys[i].first == *keys[j].first)
            {
                context.fail(std::string(keys[i].second) + " and " + keys[j].second +
                             " are both '" + *keys[i].first + "'");
            }
        }
    }

    // The schema the position topic carries has to be one this build knows, or
    // the subscriber would decode against nothing.
    if (!pub_sub::get_schema(out.position.schemaType).has_value())
    {
        context.fail("position.schema_type ('" + out.position.schemaType +
                     "') is not a schema this build knows");
    }

    if (out.match.minSigmaM > out.match.maxSigmaM)
    {
        context.fail("match.min_sigma_m is greater than match.max_sigma_m");
    }
    if (out.match.beamWidth == 0)
    {
        context.fail("match.beam_width must be at least 1");
    }

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    std::ifstream file(path);
    if (!file)
    {
        SPDLOG_ERROR("[config] cannot read {}", path);
        return false;
    }

    std::ostringstream text;
    text << file.rdbuf();
    // Delegates, so the parser the tests drive is the one the node uses.
    return parse_node_config(text.str(), out);
}

} // namespace map_match
