// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include "can/channel_id.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace can_bridge
{
namespace
{

// Errors accumulate rather than stopping at the first, so one run finds every
// typo in a file rather than one per run.
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
void read_uint(const YAML::Node& parent, const char* key, T& out, Context& context,
               const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    try
    {
        const int64_t value = node.as<int64_t>();
        if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
        {
            context.fail(fmt::format("{}.{}: {} is outside 0..{}", where, key, value,
                                     static_cast<int64_t>(std::numeric_limits<T>::max())));
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception&)
    {
        context.fail(fmt::format("{}.{}: '{}' is not a number", where, key, node.Scalar()));
    }
}

void read_bool(const YAML::Node& parent, const char* key, bool& out, Context& context,
               const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    try
    {
        out = node.as<bool>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(fmt::format("{}.{}: '{}' is not true or false", where, key, node.Scalar()));
    }
}

void read_string(const YAML::Node& parent, const char* key, std::string& out)
{
    if (const YAML::Node node = parent[key])
    {
        out = node.as<std::string>();
    }
}

// An unrecognised key is almost always a typo, and a typo that is ignored looks
// exactly like a setting that does not work.
void reject_unknown_keys(const YAML::Node& node, const std::vector<std::string>& known,
                         Context& context, const std::string& where)
{
    if (!node || !node.IsMap())
    {
        return;
    }
    for (const auto& entry : node)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end())
        {
            context.fail(fmt::format("{}: unknown key '{}'", where, key));
        }
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
    catch (const YAML::Exception& error)
    {
        SPDLOG_ERROR("[config] not valid YAML: {}", error.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] the file is empty or is not a mapping");
        return false;
    }

    reject_unknown_keys(root, { "channels", "status_key", "set_bitrate_key", "status_interval_ms",
                                "pcan_detach_kernel_driver", "continue_on_channel_error" },
                        context, "(top level)");

    read_string(root, "status_key", out.statusKey);
    read_string(root, "set_bitrate_key", out.setBitrateKey);
    read_uint(root, "status_interval_ms", out.statusIntervalMs, context, "(top level)");
    read_bool(root, "pcan_detach_kernel_driver", out.pcanDetachKernelDriver, context,
              "(top level)");
    read_bool(root, "continue_on_channel_error", out.continueOnChannelError, context,
              "(top level)");

    const YAML::Node channels = root["channels"];
    if (!channels)
    {
        SPDLOG_ERROR("[config] 'channels' is required: a bridge with no channels bridges nothing");
        return false;
    }
    if (!channels.IsSequence())
    {
        SPDLOG_ERROR("[config] 'channels' is a list, one entry per CAN bus");
        return false;
    }

    std::set<std::string> names;
    std::set<std::string> devices;
    std::set<std::string> rxKeys;

    for (size_t i = 0; i < channels.size(); ++i)
    {
        const YAML::Node node = channels[i];
        const std::string where = fmt::format("channels[{}]", i);

        if (!node.IsMap())
        {
            context.fail(fmt::format("{} is not a mapping", where));
            continue;
        }

        reject_unknown_keys(node,
                            { "name", "device", "bitrate", "data_bitrate", "sample_point_permille",
                              "data_sample_point_permille", "listen_only", "rx_key", "tx_key",
                              "rx_queue_depth", "publish_rx", "accept_tx" },
                            context, where);

        ChannelConfig channel;
        read_string(node, "name", channel.name);
        read_string(node, "device", channel.device);
        read_uint(node, "bitrate", channel.bitrateBps, context, where);
        read_uint(node, "data_bitrate", channel.dataBitrateBps, context, where);
        read_uint(node, "sample_point_permille", channel.samplePointPermille, context, where);
        read_uint(node, "data_sample_point_permille", channel.dataSamplePointPermille, context,
                  where);
        read_bool(node, "listen_only", channel.listenOnly, context, where);
        read_string(node, "rx_key", channel.rxKey);
        read_string(node, "tx_key", channel.txKey);
        read_uint(node, "rx_queue_depth", channel.rxQueueDepth, context, where);
        read_bool(node, "publish_rx", channel.publishRx, context, where);
        read_bool(node, "accept_tx", channel.acceptTx, context, where);

        if (channel.name.empty())
        {
            context.fail(fmt::format("{}.name is required: it is what the topics and the bitrate "
                                     "service call this bus",
                                     where));
        }
        if (channel.device.empty())
        {
            context.fail(fmt::format("{}.device is required, as <backend>:<device>[/<channel>]",
                                     where));
        }
        else
        {
            auto parsed = can::parse_channel_id(channel.device);
            if (!parsed.has_value())
            {
                context.fail(fmt::format("{}.device: {}", where, parsed.error().message));
            }
        }

        // Defaulting the keys from the name is what makes the common case one
        // line per channel.
        if (channel.rxKey.empty())
        {
            channel.rxKey = fmt::format("vehicle/{}/rx", channel.name);
        }
        if (channel.txKey.empty())
        {
            channel.txKey = fmt::format("vehicle/{}/tx", channel.name);
        }

        if (channel.bitrateBps == 0)
        {
            context.fail(fmt::format("{}.bitrate is 0; a bus has to have a bit rate", where));
        }

        // Two channels sharing a name would make the bitrate service ambiguous
        // and the status topic misleading.
        if (!channel.name.empty() && !names.insert(channel.name).second)
        {
            context.fail(fmt::format("{}.name '{}' is used by more than one channel", where,
                                     channel.name));
        }
        // Two channels on the same hardware would fight over it.
        //
        // Not so for a virtual bus: sharing one is what it is for, and two
        // channels on the same name is how a loopback pair is built -- which
        // is the only way to exercise the receive path without an adapter.
        const bool isVirtual = channel.device.rfind("virtual:", 0) == 0;
        if (!channel.device.empty() && !isVirtual && !devices.insert(channel.device).second)
        {
            context.fail(fmt::format("{}.device '{}' is opened by more than one channel", where,
                                     channel.device));
        }
        // Two channels publishing to one key would interleave two buses'
        // traffic into a topic nothing can separate again.
        if (channel.publishRx && !rxKeys.insert(channel.rxKey).second)
        {
            context.fail(fmt::format("{}.rx_key '{}' is published by more than one channel", where,
                                     channel.rxKey));
        }

        out.channels.push_back(std::move(channel));
    }

    if (out.channels.empty())
    {
        SPDLOG_ERROR("[config] no channels were configured");
        return false;
    }

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    std::ifstream in(path);
    if (!in)
    {
        SPDLOG_ERROR("[config] cannot read {}", path);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_node_config(buffer.str(), out);
}

} // namespace can_bridge
