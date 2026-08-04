// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the runtime keypad node is configured.
//
// The node predates the repository's `--config` convention and used ad-hoc
// flags. What pushed it over is not tidiness: a keypad's node ID, its COB-IDs
// and its brightness defaults are vehicle configuration, and a vehicle that
// carries two keypads needs two of these -- which a flag set cannot express and
// a file can.
//
// Everything here has a working default, so the file is small; what it does not
// have is anything about the object dictionary. This node speaks PDO only.
// Changing what the keypad *is* belongs to grayhill_keypad_reconfigure, which
// runs once and exits.
#ifndef GRAYHILL_NODE_CONFIG_H
#define GRAYHILL_NODE_CONFIG_H

#include <cstdint>
#include <string>

namespace grayhill
{

struct NodeConfig
{
    // Grayhill's factory default. MoTeC-configured keypads are usually moved.
    uint8_t nodeId { 0x0A };

    std::string rxKey { "vehicle/can0/rx" };
    std::string txKey { "vehicle/can0/tx" };

    // Where this keypad's own topics live, so a second keypad does not collide
    // with the first.
    std::string topicPrefix { "nodes/grayhill_keypad" };

    // Brightness applied once the keypad is operational. Both channels are sent
    // together because RPDO2 carries them together.
    uint16_t indicatorBrightness { 255 };
    uint16_t backlightBrightness { 0 };

    // Take the keypad through pre-operational to operational at startup. Off
    // for a keypad managed by another CANopen master on the same bus, where a
    // second master issuing NMT would fight it.
    bool driveNmt { true };

    // Producer heartbeat time in milliseconds, written over SDO at startup, or
    // 0 to leave the keypad's own setting alone.
    //
    // Not simply "0 means disable": disabling a heartbeat is a change to
    // non-volatile-backed device state, and this node does not make those.
    uint16_t heartbeatMs { 0 };

    // How long to wait for zenoh peering before sending anything. Publishing
    // into a session whose peers have not connected yet drops the frames
    // silently, which is what made the old startup burst unreliable.
    uint32_t startupDelayMs { 500 };
};

// Reads the file. Returns false and logs what was wrong; the caller should
// refuse to start rather than fall back to defaults, since silently dropping
// whatever the file was configuring is the opposite of why it is required.
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace grayhill

#endif // GRAYHILL_NODE_CONFIG_H
