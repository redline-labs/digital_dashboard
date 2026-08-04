// SPDX-License-Identifier: GPL-3.0-or-later

#include "reconfigure_config.h"

#include <spdlog/fmt/fmt.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace grayhill
{
namespace
{

// A key that may or may not be there. Present and unreadable is an error;
// absent leaves the optional empty, which downstream means "do not write it".
template <typename T>
void read_optional(const YAML::Node& parent, const char* key, std::optional<T>& out,
                   std::vector<std::string>& errors, const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }

    try
    {
        // Read wide and range-check, so `node_id: 300` is reported as out of
        // range rather than silently becoming 44.
        const int64_t value = node.as<int64_t>();
        if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
        {
            errors.push_back(fmt::format("{}.{}: {} is outside the range 0..{}", where, key, value,
                                         static_cast<int64_t>(std::numeric_limits<T>::max())));
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception&)
    {
        errors.push_back(
            fmt::format("{}.{}: '{}' is not a number", where, key, node.Scalar()));
    }
}

template <typename T>
bool read_required(const YAML::Node& parent, const char* key, T& out,
                   std::vector<std::string>& errors, const std::string& where)
{
    std::optional<T> value;
    read_optional(parent, key, value, errors, where);
    if (!value.has_value())
    {
        if (!parent[key])
        {
            errors.push_back(fmt::format("{}.{} is required", where, key));
        }
        return false;
    }
    out = *value;
    return true;
}

void read_bool(const YAML::Node& parent, const char* key, bool& out,
               std::vector<std::string>& errors, const std::string& where)
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
        errors.push_back(
            fmt::format("{}.{}: '{}' is not true or false", where, key, node.Scalar()));
    }
}

// A key that is not one this tool knows about is almost always a typo, and a
// typo in a file that writes non-volatile memory is worth stopping for. The
// alternative -- ignoring it -- means `backlight_scaler: 254` does nothing at
// all and says nothing about it.
void reject_unknown_keys(const YAML::Node& node, const std::vector<std::string>& known,
                         std::vector<std::string>& errors, const std::string& where)
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
            errors.push_back(fmt::format("{}: unknown key '{}'", where, key));
        }
    }
}

} // namespace

std::optional<ReconfigConfig> parse_reconfig_config(const std::string& yaml,
                                                    std::vector<std::string>& errors)
{
    YAML::Node root;
    try
    {
        root = YAML::Load(yaml);
    }
    catch (const YAML::Exception& error)
    {
        errors.push_back(fmt::format("not valid YAML: {}", error.what()));
        return std::nullopt;
    }

    if (!root || !root.IsMap())
    {
        errors.push_back("the file is empty or is not a mapping");
        return std::nullopt;
    }

    reject_unknown_keys(root, { "current", "target", "store", "reset_after" }, errors, "(top level)");

    ReconfigConfig config;

    // --- current ------------------------------------------------------------
    const YAML::Node current = root["current"];
    if (!current)
    {
        errors.push_back("'current' is required: the tool has to know how to reach the keypad "
                         "before it can change anything");
    }
    else
    {
        reject_unknown_keys(current, { "node_id", "bitrate" }, errors, "current");
        read_required(current, "node_id", config.current.nodeId, errors, "current");

        uint32_t bitrate = 0;
        if (read_required(current, "bitrate", bitrate, errors, "current"))
        {
            // Written in bit/s in the file because that is how everyone talks
            // about it; carried in kbit/s because that is what LSS indexes.
            config.current.bitrateKbps = bitrate >= 1000 ? bitrate / 1000 : bitrate;
        }
    }

    if (config.current.nodeId < 1 || config.current.nodeId > 127)
    {
        if (current && current["node_id"])
        {
            errors.push_back(fmt::format("current.node_id {} is outside the range 1..127",
                                         config.current.nodeId));
        }
    }

    // --- target -------------------------------------------------------------
    const YAML::Node target = root["target"];
    if (target)
    {
        reject_unknown_keys(target,
                            { "node_id", "bitrate", "cob_ids", "heartbeat_ms", "event_timer_ms",
                              "inhibit_time_us", "transmission_type", "indicator_scalar",
                              "backlight_scalar", "motec_compatible" },
                            errors, "target");

        read_optional(target, "node_id", config.target.nodeId, errors, "target");

        std::optional<uint32_t> bitrate;
        read_optional(target, "bitrate", bitrate, errors, "target");
        if (bitrate.has_value())
        {
            config.target.bitrateKbps = *bitrate >= 1000 ? *bitrate / 1000 : *bitrate;
        }

        const YAML::Node cobIds = target["cob_ids"];
        if (cobIds)
        {
            reject_unknown_keys(cobIds, { "buttons", "indicators", "brightness" }, errors,
                                "target.cob_ids");
            read_optional(cobIds, "buttons", config.target.cobIds.buttons, errors,
                          "target.cob_ids");
            read_optional(cobIds, "indicators", config.target.cobIds.indicators, errors,
                          "target.cob_ids");
            read_optional(cobIds, "brightness", config.target.cobIds.brightness, errors,
                          "target.cob_ids");
        }

        read_optional(target, "heartbeat_ms", config.target.heartbeatMs, errors, "target");
        read_optional(target, "event_timer_ms", config.target.eventTimerMs, errors, "target");
        read_optional(target, "inhibit_time_us", config.target.inhibitTimeUs, errors, "target");
        read_optional(target, "transmission_type", config.target.transmissionType, errors,
                      "target");
        read_optional(target, "indicator_scalar", config.target.indicatorScalar, errors, "target");
        read_optional(target, "backlight_scalar", config.target.backlightScalar, errors, "target");
        read_bool(target, "motec_compatible", config.target.motecCompatible, errors, "target");
    }

    read_bool(root, "store", config.store, errors, "(top level)");
    read_bool(root, "reset_after", config.resetAfter, errors, "(top level)");

    // --- the shorthand, and what it conflicts with --------------------------
    if (config.target.motecCompatible)
    {
        constexpr uint8_t kMarker = 0xFE;

        if (config.target.transmissionType.has_value()
            && *config.target.transmissionType != kMarker)
        {
            errors.push_back(fmt::format(
                "target.motec_compatible needs transmission_type 0xFE, but transmission_type is "
                "set to 0x{:02X}; say one or the other",
                *config.target.transmissionType));
        }
        if (config.target.backlightScalar.has_value() && *config.target.backlightScalar != kMarker)
        {
            errors.push_back(fmt::format(
                "target.motec_compatible needs backlight_scalar 0xFE, but backlight_scalar is set "
                "to 0x{:02X}; say one or the other",
                *config.target.backlightScalar));
        }

        config.target.transmissionType = kMarker;
        config.target.backlightScalar = kMarker;
    }

    // A COB-ID is eleven bits. Anything wider is a typo -- most likely the
    // control bits from a read-back value pasted straight back in.
    auto checkCobId = [&](const char* name, const std::optional<uint32_t>& value)
    {
        if (value.has_value() && *value > 0x7FF)
        {
            errors.push_back(fmt::format(
                "target.cob_ids.{}: 0x{:X} is wider than an 11-bit CAN identifier; the tool adds "
                "the CiA control bits itself, so give the identifier alone",
                name, *value));
        }
    };
    checkCobId("buttons", config.target.cobIds.buttons);
    checkCobId("indicators", config.target.cobIds.indicators);
    checkCobId("brightness", config.target.cobIds.brightness);

    if (config.target.nodeId.has_value()
        && (*config.target.nodeId < 1 || *config.target.nodeId > 127))
    {
        errors.push_back(fmt::format("target.node_id {} is outside the range 1..127",
                                     *config.target.nodeId));
    }

    if (!errors.empty())
    {
        return std::nullopt;
    }
    return config;
}

std::optional<ReconfigConfig> load_reconfig_config(const std::string& path,
                                                   std::vector<std::string>& errors)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        errors.push_back(fmt::format("no such file: {}", path));
        return std::nullopt;
    }

    try
    {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return parse_reconfig_config(buffer.str(), errors);
    }
    catch (const std::exception& error)
    {
        errors.push_back(fmt::format("cannot read {}: {}", path, error.what()));
        return std::nullopt;
    }
}

} // namespace grayhill
