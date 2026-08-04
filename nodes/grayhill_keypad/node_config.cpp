// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace grayhill
{
namespace
{

bool ok = true;

template <typename T>
void assign_if_present(const YAML::Node& parent, const char* key, T& out, const char* where)
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
            SPDLOG_ERROR("[config] {}.{}: {} is outside 0..{}", where, key, value,
                         static_cast<int64_t>(std::numeric_limits<T>::max()));
            ok = false;
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception&)
    {
        SPDLOG_ERROR("[config] {}.{}: '{}' is not a number", where, key, node.Scalar());
        ok = false;
    }
}

void assign_string(const YAML::Node& parent, const char* key, std::string& out)
{
    if (const YAML::Node node = parent[key])
    {
        out = node.as<std::string>();
    }
}

void assign_bool(const YAML::Node& parent, const char* key, bool& out, const char* where)
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
        SPDLOG_ERROR("[config] {}.{}: '{}' is not true or false", where, key, node.Scalar());
        ok = false;
    }
}

// A key this node does not know is almost always a typo, and a typo that is
// silently ignored looks exactly like a setting that does not work.
void reject_unknown_keys(const YAML::Node& node, const std::vector<std::string>& known,
                         const char* where)
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
            SPDLOG_ERROR("[config] {}: unknown key '{}'", where, key);
            ok = false;
        }
    }
}

} // namespace

bool load_node_config(const std::string& path, NodeConfig& out)
{
    ok = true;

    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path);
    }
    catch (const YAML::Exception& error)
    {
        SPDLOG_ERROR("[config] cannot read {}: {}", path, error.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] {} is empty or is not a mapping", path);
        return false;
    }

    reject_unknown_keys(root, { "keypad", "zenoh", "brightness" }, "(top level)");

    if (const YAML::Node keypad = root["keypad"])
    {
        reject_unknown_keys(keypad, { "node_id", "drive_nmt", "heartbeat_ms", "startup_delay_ms" },
                            "keypad");
        assign_if_present(keypad, "node_id", out.nodeId, "keypad");
        assign_bool(keypad, "drive_nmt", out.driveNmt, "keypad");
        assign_if_present(keypad, "heartbeat_ms", out.heartbeatMs, "keypad");
        assign_if_present(keypad, "startup_delay_ms", out.startupDelayMs, "keypad");
    }

    if (const YAML::Node zenoh = root["zenoh"])
    {
        reject_unknown_keys(zenoh, { "rx_key", "tx_key", "topic_prefix" }, "zenoh");
        assign_string(zenoh, "rx_key", out.rxKey);
        assign_string(zenoh, "tx_key", out.txKey);
        assign_string(zenoh, "topic_prefix", out.topicPrefix);
    }

    if (const YAML::Node brightness = root["brightness"])
    {
        reject_unknown_keys(brightness, { "indicator", "backlight" }, "brightness");
        assign_if_present(brightness, "indicator", out.indicatorBrightness, "brightness");
        assign_if_present(brightness, "backlight", out.backlightBrightness, "brightness");
    }

    if (out.nodeId < 1 || out.nodeId > 127)
    {
        SPDLOG_ERROR("[config] keypad.node_id {} is outside the CANopen range 1..127", out.nodeId);
        ok = false;
    }

    // 0x6411:01's declared range is 1..255. Zero is not "off", it is out of
    // range, and the device aborts a write of it.
    if (out.indicatorBrightness < 1 || out.indicatorBrightness > 255)
    {
        SPDLOG_ERROR("[config] brightness.indicator {} is outside the device's range 1..255",
                     out.indicatorBrightness);
        ok = false;
    }
    if (out.backlightBrightness > 255)
    {
        SPDLOG_ERROR("[config] brightness.backlight {} is outside the device's range 0..255",
                     out.backlightBrightness);
        ok = false;
    }

    return ok;
}

} // namespace grayhill
