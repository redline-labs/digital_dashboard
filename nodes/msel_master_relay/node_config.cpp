// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include "msel/protocol.h"
#include "pub_sub/topic_key.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace msel_node
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
void read_uint(const YAML::Node& parent, const char* key, T& out, Context& context)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    try
    {
        // Read as a string first so that 0x6E4 works. CAN identifiers are
        // written in hex everywhere else -- in the manual, on the bus, in every
        // other tool -- and a config file that silently took 0x6E4 as 0 would be
        // a trap.
        const std::string text = node.Scalar();
        size_t consumed = 0u;
        const int64_t value = std::stoll(text, &consumed, 0);
        if (consumed != text.size())
        {
            context.fail(fmt::format("{}: '{}' is not a number", key, text));
            return;
        }
        if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
        {
            context.fail(fmt::format("{}: {} is outside 0..{}", key, value,
                                     static_cast<int64_t>(std::numeric_limits<T>::max())));
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const std::exception&)
    {
        context.fail(fmt::format("{}: '{}' is not a number", key, node.Scalar()));
    }
}

void read_bool(const YAML::Node& parent, const char* key, bool& out, Context& context)
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
        context.fail(fmt::format("{}: '{}' is not true or false", key, node.Scalar()));
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
                         Context& context)
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
            context.fail(fmt::format("unknown key '{}'", key));
        }
    }
}

void check_topic_key(const std::string& value, const char* key, Context& context)
{
    if (value.empty())
    {
        context.fail(fmt::format("{}: cannot be empty", key));
        return;
    }
    const std::string problem = pub_sub::topicKeyProblem(value);
    if (!problem.empty())
    {
        context.fail(fmt::format("{}: '{}' {}", key, value, problem));
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

    reject_unknown_keys(root,
                        { "rx_key", "tx_key", "topic_prefix", "base_address", "kill_address",
                          "allow_can_kill", "command_timeout_ms", "status_interval_ms" },
                        context);

    read_string(root, "rx_key", out.rxKey);
    read_string(root, "tx_key", out.txKey);
    read_string(root, "topic_prefix", out.topicPrefix);
    read_uint(root, "base_address", out.baseAddress, context);
    read_uint(root, "kill_address", out.killAddress, context);
    read_bool(root, "allow_can_kill", out.allowCanKill, context);
    read_uint(root, "command_timeout_ms", out.commandTimeoutMs, context);
    read_uint(root, "status_interval_ms", out.statusIntervalMs, context);

    check_topic_key(out.rxKey, "rx_key", context);
    check_topic_key(out.txKey, "tx_key", context);
    check_topic_key(out.topicPrefix, "topic_prefix", context);

    if (out.rxKey == out.txKey)
    {
        context.fail(fmt::format(
            "rx_key and tx_key are both '{}'. Every command this node sends would come straight "
            "back to its own decoder",
            out.rxKey));
    }

    // A window this short cannot contain an answer that had to queue behind the
    // relay's own 10Hz telemetry, so every accepted command would be reported
    // as unanswered -- a "no" that is wrong, which is worse than a slow "yes".
    // Zero is rejected rather than read as "do not wait": there is no longer
    // anywhere else for the answer to appear, so not waiting means never
    // learning the outcome at all.
    constexpr uint32_t kMinCommandTimeoutMs = 100u;
    constexpr uint32_t kMaxCommandTimeoutMs = 10000u;
    if (out.commandTimeoutMs < kMinCommandTimeoutMs ||
        out.commandTimeoutMs > kMaxCommandTimeoutMs)
    {
        context.fail(fmt::format(
            "command_timeout_ms: {} is outside {}..{}. Below the floor an accepted command reads "
            "as unanswered; above the ceiling the caller's own query gives up first and reports a "
            "failure instead of the answer",
            out.commandTimeoutMs, kMinCommandTimeoutMs, kMaxCommandTimeoutMs));
    }

    // The same rule the device itself enforces, applied here so that a bad
    // address is a refusal to start rather than a node that silently decodes
    // nothing.
    if (const auto valid = msel::validateBaseAddress(out.baseAddress); !valid)
    {
        context.fail(fmt::format("base_address: {}", valid.error().message));
    }

    if (out.killAddress > msel::kMaxStandardId)
    {
        context.fail(fmt::format("kill_address: 0x{:X} is wider than the 11 bits the device accepts",
                                 out.killAddress));
    }

    if (out.killAddress == msel::kConfigCommandId)
    {
        context.fail(fmt::format(
            "kill_address: 0x{:X} is the configuration identifier, so every configuration command "
            "would also read as a shutdown request",
            out.killAddress));
    }

    // A kill address on one of the three identifiers the relay transmits would
    // have this node writing onto an identifier it also decodes as telemetry.
    //
    // Note that base+2 is fine, and is in fact the factory default: the relay
    // transmits on base, base+1 and base+3 only, which is precisely why MSEL
    // picked the gap at base+2 (0x6E6) for the kill frame. Rejecting the whole
    // base..base+3 span would refuse the vendor's own recommended setup.
    const msel::Addresses addresses { .base = out.baseAddress };
    if (out.killAddress == addresses.status() || out.killAddress == addresses.info() ||
        out.killAddress == addresses.switchState())
    {
        context.fail(fmt::format(
            "kill_address: 0x{:X} is one of the identifiers the relay transmits on "
            "(0x{:X}, 0x{:X}, 0x{:X})",
            out.killAddress, addresses.status(), addresses.info(), addresses.switchState()));
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

} // namespace msel_node
