// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which MSEL Master Relay this node is talking to, and what it is allowed to do
// to it.
//
// One relay per process. The base address is configurable on the device
// precisely so that several can share a bus, and running a second node with a
// different `base_address` and `topic_prefix` is how you watch two of them --
// the same way a second CAN bus gets a second channel entry rather than a
// second bridge.
//
// The one setting here that is not a convenience is `allow_can_kill`. The relay
// accepts a frame that isolates the battery and stops the engine, and this node
// can send it. That capability is off unless it is turned on here, and even
// then a caller has to supply a confirmation token -- see kCanKillConfirmToken.
// Two gates rather than one, because a config file left permissive on a bench
// should not become a way to stop a moving car with a single service call.
#ifndef MSEL_MASTER_RELAY_NODE_CONFIG_H
#define MSEL_MASTER_RELAY_NODE_CONFIG_H

#include <cstdint>
#include <string>

namespace msel_node
{

// What MselCanKillRequest.confirm has to contain, exactly, before a remote
// shutdown is transmitted. Deliberately not something a client would send by
// accident while exploring the service with `inspect call`.
inline constexpr const char* kCanKillConfirmToken = "ISOLATE-THE-BATTERY";

struct NodeConfig
{
    // Where raw CAN frames arrive from and are handed back to. These are the
    // can_bridge topics for whichever bus the relay is on.
    std::string rxKey { "vehicle/can0/rx" };
    std::string txKey { "vehicle/can0/tx" };

    // Everything this node publishes and serves hangs off here.
    std::string topicPrefix { "nodes/msel_master_relay" };

    // The relay's base CAN address. Its three messages are at base, base+1 and
    // base+3. Factory default is 0x6E4; a relay that has been re-addressed will
    // be silent on the default, which is the first thing to check when the
    // status topic never updates.
    uint32_t baseAddress { 0x6E4u };

    // Where a remote shutdown frame would be sent. Has to match what the relay
    // was configured to listen on -- it is not derived from the base address.
    uint32_t killAddress { 0x6E6u };

    // Permit this node to transmit a remote shutdown at all. Off by default.
    bool allowCanKill { false };

    // How often the status topic is republished even when nothing changed. Zero
    // publishes only on receipt, which is the right choice on a bus where the
    // relay is already transmitting at 10Hz; a non-zero value is for keeping a
    // late subscriber from waiting.
    uint32_t statusIntervalMs { 0 };
};

// Reads the file. Returns false and logs every problem it found rather than the
// first, so a config with three typos takes one run to fix.
bool load_node_config(const std::string& path, NodeConfig& out);

// Exposed for tests.
bool parse_node_config(const std::string& yaml, NodeConfig& out);

} // namespace msel_node

#endif // MSEL_MASTER_RELAY_NODE_CONFIG_H
