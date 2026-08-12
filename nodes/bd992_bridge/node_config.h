// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bd992 node's YAML configuration.
//
// Same shape as nodes/can_bridge: plain structs with in-class defaults, a
// parse that accumulates every error rather than stopping at the first, and a
// string-taking overload so the parser is testable without a file.
#ifndef BD992_NODE_CONFIG_H
#define BD992_NODE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "bd992/output_config.h"
#include "gsof/commands.h"
#include "gsof/records.h"

namespace bd992_node
{

// One GSOF record the receiver should emit, and how often.
struct OutputEntry
{
    gsof::RecordType record { gsof::RecordType::LatLongHeight };
    gsof::appfile::Frequency rate { gsof::appfile::Frequency::Hz1 };
    std::uint8_t offsetSeconds { 0 };
};

struct ReceiverConfig
{
    std::string host;

    // The IP socket the receiver is configured to output GSOF on, and the one
    // used for command and report. Two connections: see
    // libs/bd992/include/bd992/control_client.h for why.
    //
    // WHICH PORT THE COMMAND INTERFACE LISTENS ON IS NOT DOCUMENTED in the
    // ICD, which is why both are settings and why `--probe` exists. If a
    // single socket turns out to serve both, point them at the same number.
    std::uint16_t streamPort { 5017 };
    std::uint16_t controlPort { 5018 };

    std::uint32_t connectTimeoutMs { 2000 };

    // Tried in order, then the last repeats. Capped rather than doubling
    // forever so a receiver that comes back after an hour is picked up in
    // seconds.
    std::vector<std::uint32_t> reconnectBackoffMs { 250, 500, 1000, 2000, 5000 };
};

enum class ConfigMode
{
    // Read the receiver's configuration, report what differs, change nothing.
    ReportOnly,
    // Read first, then write only what actually drifted.
    Enforce,
};

const char* to_string(ConfigMode mode);

struct ConfigurationConfig
{
    ConfigMode mode { ConfigMode::Enforce };

    // Zero-based, as the wire has it: 20, 21, 22 are the first three IP
    // sockets. This must be the port `streamPort` connects to, or the node
    // will diligently configure outputs onto a socket nobody is reading.
    std::uint8_t portIndex { 20 };

    bd992::PortPolicy portPolicy { bd992::PortPolicy::Additive };

    // Which stored application file holds the running configuration. The ICD
    // documents index 0 as the factory defaults and says nothing about the
    // rest, so this is a setting rather than a constant.
    std::uint16_t applicationFileIndex { 1 };

    // How often to re-read and re-compare. Zero checks only on connect.
    std::uint32_t recheckIntervalS { 60 };

    std::uint32_t replyTimeoutMs { 3000 };

    // Gates the send_command service. Off by default: an arbitrary ICD packet
    // can leave a receiver unreachable.
    bool allowRawCommands { false };

    std::vector<OutputEntry> outputs;
};

struct PublishConfig
{
    std::string topicPrefix { "nodes/bd992" };
    std::string statusKey { "nodes/bd992/status" };
    std::uint32_t statusIntervalMs { 1000 };

    // Publish records not in GSOF_RECORD_TABLE on <prefix>/gsof/raw. On by
    // default: a receiver emitting a record we do not model is otherwise
    // indistinguishable from one that is silent.
    bool publishUnknownRecords { true };
};

struct NodeConfig
{
    ReceiverConfig receiver;
    ConfigurationConfig configuration;
    PublishConfig publish;
};

// Both report every problem they find before returning false, so a config with
// three mistakes takes one run to fix rather than three.
bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

// The desired output list, as the diff wants it.
std::vector<bd992::OutputMessage> desired_outputs(const ConfigurationConfig& config);

} // namespace bd992_node

#endif // BD992_NODE_CONFIG_H
