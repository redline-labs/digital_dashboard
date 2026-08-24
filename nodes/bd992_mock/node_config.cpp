// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_config.h"

#include <array>
#include <fstream>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

#include <yaml-cpp/yaml.h>

#include "pub_sub/topic_key.h"

namespace bd992_mock
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
    // refuses and nothing subscribes, and a query goes to nobody. Checking here
    // turns a typo into a startup error rather than a topic nobody can reach.
    if (const std::string problem = pub_sub::topicKeyProblem(key); !problem.empty())
    {
        context.fail(std::string(field) + " ('" + key + "'): " + problem);
    }
}

void positive(double value, const char* field, Context& context)
{
    if (!(value > 0.0))
    {
        context.fail(std::string(field) + " must be greater than zero");
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

    if (const YAML::Node node = root["services"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("services must be a mapping");
        }
        else
        {
            readString(node, "graph_info_key", out.services.graphInfoKey, context, "services");
            readString(node, "route_key", out.services.routeKey, context, "services");
            readString(node, "nearest_key", out.services.nearestKey, context, "services");
            readString(node, "track_catalog_key", out.services.trackCatalogKey, context,
                       "services");
            readString(node, "track_detail_key", out.services.trackDetailKey, context, "services");
            readNumber(node, "request_timeout_ms", out.services.requestTimeoutMs, context,
                       "services");
            readString(node, "graph", out.services.graph, context, "services");
            readString(node, "trackset", out.services.trackset, context, "services");
        }
    }

    if (const YAML::Node node = root["vehicle"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("vehicle must be a mapping");
        }
        else
        {
            readNumber(node, "cruise_speed_mps", out.vehicle.cruiseSpeedMps, context, "vehicle");
            readNumber(node, "accel_mps2", out.vehicle.accelMps2, context, "vehicle");
            readNumber(node, "brake_mps2", out.vehicle.brakeMps2, context, "vehicle");
            readNumber(node, "lateral_accel_mps2", out.vehicle.lateralAccelMps2, context,
                       "vehicle");
            readNumber(node, "heading_lookahead_m", out.vehicle.headingLookaheadM, context,
                       "vehicle");
            readNumber(node, "curvature_baseline_m", out.vehicle.curvatureBaselineM, context,
                       "vehicle");
        }
    }

    if (const YAML::Node node = root["publish"]; node)
    {
        if (!node.IsMap())
        {
            context.fail("publish must be a mapping");
        }
        else
        {
            readString(node, "topic_prefix", out.publish.topicPrefix, context, "publish");
            readNumber(node, "rate_hz", out.publish.rateHz, context, "publish");
            readNumber(node, "ellipsoid_height_m", out.publish.ellipsoidHeightM, context,
                       "publish");
            readNumber(node, "position_noise_m", out.publish.positionNoiseM, context, "publish");
            readNumber(node, "noise_seed", out.publish.noiseSeed, context, "publish");
        }
    }

    checkKey(out.services.graphInfoKey, "services.graph_info_key", context);
    checkKey(out.services.routeKey, "services.route_key", context);
    checkKey(out.services.nearestKey, "services.nearest_key", context);
    checkKey(out.services.trackCatalogKey, "services.track_catalog_key", context);
    checkKey(out.services.trackDetailKey, "services.track_detail_key", context);
    checkKey(out.publish.topicPrefix, "publish.topic_prefix", context);

    // Two services on one key answer each other's questions: a request goes to
    // whichever queryable replies first, and capnp decodes the wrong reply into
    // plausible numbers rather than an error.
    const std::array<std::pair<const std::string*, const char*>, 5> keys { {
        { &out.services.graphInfoKey, "services.graph_info_key" },
        { &out.services.routeKey, "services.route_key" },
        { &out.services.nearestKey, "services.nearest_key" },
        { &out.services.trackCatalogKey, "services.track_catalog_key" },
        { &out.services.trackDetailKey, "services.track_detail_key" },
    } };
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        for (std::size_t j = i + 1; j < keys.size(); ++j)
        {
            if (*keys[i].first == *keys[j].first)
            {
                context.fail(std::string(keys[i].second) + " and " + keys[j].second +
                             " are both '" + *keys[i].first + "'");
            }
        }
    }

    if (out.publish.rateHz == 0)
    {
        context.fail("publish.rate_hz must be at least 1");
    }
    if (out.services.requestTimeoutMs == 0)
    {
        context.fail("services.request_timeout_ms must be greater than zero");
    }
    if (out.publish.positionNoiseM < 0.0)
    {
        context.fail("publish.position_noise_m cannot be negative");
    }

    positive(out.vehicle.cruiseSpeedMps, "vehicle.cruise_speed_mps", context);
    positive(out.vehicle.accelMps2, "vehicle.accel_mps2", context);
    positive(out.vehicle.brakeMps2, "vehicle.brake_mps2", context);
    positive(out.vehicle.lateralAccelMps2, "vehicle.lateral_accel_mps2", context);
    positive(out.vehicle.headingLookaheadM, "vehicle.heading_lookahead_m", context);

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

} // namespace bd992_mock
