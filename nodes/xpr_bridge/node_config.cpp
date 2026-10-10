// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include "node_config/reader.h"

#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <fstream>
#include <limits>

#include "pub_sub/topic_key.h"

namespace xpr_node
{

namespace
{

using node_config::Context;
using node_config::readUint;
using node_config::readString;
using node_config::readBool;

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

    node_config::checkTopicKey(out.topicPrefix, "publish.topic_prefix", context);
    node_config::checkTopicKey(out.statusKey, "publish.status_key", context);
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
