// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <fstream>
#include <limits>

#include "pub_sub/topic_key.h"

namespace xpr_node
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

template <typename T>
void readUint(const YAML::Node& parent, const char* key, T& out, Context& context, const std::string& where)
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
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

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
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

void readBool(const YAML::Node& parent, const char* key, bool& out, Context& context,
              const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        out = parent[key].as<bool>();
    }
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

void checkTopicKey(const std::string& key, const char* where, Context& context)
{
    // Checked here rather than at the publisher, because a bad character does
    // not fail loudly on the bus -- `*` and `?` are rejected by zenoh outright
    // and `@` makes a segment verbatim, so a topic containing one is simply
    // never seen by any wildcard subscriber.
    if (const std::string problem = pub_sub::topicKeyProblem(key); !problem.empty())
    {
        context.fail(std::string(where) + ": " + problem);
    }
}

void parseRadio(const YAML::Node& node, RadioConfig& out, Context& context)
{
    if (!node)
    {
        return;
    }

    readString(node, "host", out.host, context, "radio");
    readUint(node, "port", out.port, context, "radio");
    readUint(node, "connect_timeout_ms", out.connectTimeoutMs, context, "radio");
    readUint(node, "reply_timeout_ms", out.replyTimeoutMs, context, "radio");

    if (node["reconnect_backoff_ms"])
    {
        try
        {
            std::vector<std::uint32_t> backoff;
            for (const YAML::Node& entry : node["reconnect_backoff_ms"])
            {
                backoff.push_back(entry.as<std::uint32_t>());
            }

            if (backoff.empty())
            {
                context.fail("radio.reconnect_backoff_ms: must not be empty");
            }
            else
            {
                out.reconnectBackoffMs = std::move(backoff);
            }
        }
        catch (const YAML::Exception& e)
        {
            context.fail(std::string("radio.reconnect_backoff_ms: ") + e.what());
        }
    }

    if (out.host.empty())
    {
        context.fail("radio.host: must not be empty");
    }
    if (out.port == 0)
    {
        context.fail("radio.port: must not be zero");
    }
}

void parseControl(const YAML::Node& node, ControlConfig& out, Context& context)
{
    if (!node)
    {
        return;
    }

    readBool(node, "allow_channel_change", out.allowChannelChange, context, "control");
}

void parsePublish(const YAML::Node& node, PublishConfig& out, Context& context)
{
    if (node)
    {
        readString(node, "topic_prefix", out.topicPrefix, context, "publish");
        readString(node, "status_key", out.statusKey, context, "publish");
        readUint(node, "status_interval_ms", out.statusIntervalMs, context, "publish");
        readBool(node, "publish_display", out.publishDisplay, context, "publish");
        readBool(node, "publish_unknown_broadcasts", out.publishUnknownBroadcasts, context, "publish");
    }

    if (out.topicPrefix.empty())
    {
        context.fail("publish.topic_prefix: must not be empty");
        return;
    }

    // Derived rather than duplicated: a status key that has drifted from the
    // prefix is a topic nobody is looking at.
    if (out.statusKey.empty())
    {
        out.statusKey = out.topicPrefix + "/status";
    }

    if (out.statusIntervalMs == 0)
    {
        context.fail("publish.status_interval_ms: must not be zero");
    }

    checkTopicKey(out.topicPrefix, "publish.topic_prefix", context);
    checkTopicKey(out.statusKey, "publish.status_key", context);
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
        SPDLOG_ERROR("[config] the top level must be a map");
        return false;
    }

    parseRadio(root["radio"], out.radio, context);
    parseControl(root["control"], out.control, context);
    parsePublish(root["publish"], out.publish, context);

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

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_node_config(text, out);
}

} // namespace xpr_node
