// SPDX-License-Identifier: GPL-3.0-or-later
//
// The xpr_bridge node's YAML configuration.
//
// Same shape as nodes/bd992_bridge and nodes/can_bridge: plain structs with
// in-class defaults, a parse that accumulates every error rather than stopping
// at the first, and a string-taking overload so the parser is testable without
// a file.

#ifndef XPR_NODE_CONFIG_H
#define XPR_NODE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace xpr_node
{

struct RadioConfig
{
    // The radio's own address on the RNDIS link. Configurable on the radio,
    // but this is what Motorola's own tooling defaults to.
    std::string host { "192.168.10.1" };

    // 8002 is the plaintext XNL port and the primary path. 8003 is the
    // optional secure session, which is closed on the radio this was
    // validated against and is not implemented here.
    std::uint16_t port { 8002 };

    std::uint32_t connectTimeoutMs { 2000 };

    // How long a query waits for its answer. This is a DEADLINE for the whole
    // exchange, not a budget per frame read, so a radio pushing broadcasts
    // cannot stretch it.
    std::uint32_t replyTimeoutMs { 2000 };

    // Tried in order, then the last repeats. Nothing sleeps on these: a failed
    // attempt schedules the next one and returns, so the node keeps serving
    // while the radio is unplugged.
    std::vector<std::uint32_t> reconnectBackoffMs { 250, 500, 1000, 2000, 5000 };
};

struct ControlConfig
{
    // Gates the set_channel service. OFF by default: this is somebody's radio
    // and a service call moves it off the channel they are listening to.
    // Everything else this node does is read-only.
    bool allowChannelChange { false };
};

struct PublishConfig
{
    std::string topicPrefix { "nodes/xpr" };

    // Derived from topicPrefix when the file does not say otherwise.
    std::string statusKey;

    std::uint32_t statusIntervalMs { 1000 };

    // Mirror the radio's own display. On by default: it is the only view of
    // what the operator is actually seeing, and the channel NAMES live in the
    // codeplug, which this build does not read -- the display is where a name
    // can be had at all.
    bool publishDisplay { true };

    // Publish the 0xB4xx broadcasts this build does not model, as bytes. On
    // by default, for the reason bd992 publishes unknown GSOF records: a radio
    // saying something new is otherwise indistinguishable from a silent one.
    bool publishUnknownBroadcasts { true };
};

struct NodeConfig
{
    RadioConfig radio;
    ControlConfig control;
    PublishConfig publish;
};

// Both report every problem they find before returning false, so a config with
// three mistakes takes one run to fix rather than three.
bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace xpr_node

#endif // XPR_NODE_CONFIG_H
