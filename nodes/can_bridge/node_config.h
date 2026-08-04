// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the CAN bridge is asked to open, and where each channel's traffic goes.
//
// One process, N channels. That is not just tidiness: a PCAN-USB Pro FD is one
// USB handle serving two CAN channels, and two processes cannot share it -- the
// second open fails. So the thing that owns the hardware has to be the thing
// that owns all of its channels, and the config is a list rather than a set of
// flags.
//
// Each channel gets a `name`, which is what the topics and the bitrate service
// use. The name is deliberately separate from the `device`: swapping a PCAN
// dongle for a SocketCAN interface should change one line here and nothing
// anywhere else.
#ifndef CAN_BRIDGE_NODE_CONFIG_H
#define CAN_BRIDGE_NODE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace can_bridge
{

struct ChannelConfig
{
    // How this bus is referred to everywhere above the hardware.
    std::string name;

    // Which adapter, as `<backend>:<device>[/<channel>]`:
    //
    //     socketcan:can0     the kernel interface named can0
    //     pcan:0             the first PCAN adapter found, its channel 0
    //     pcan:0/1           that same adapter, channel 1
    //     pcan:LSN00123/1    the adapter with that serial, channel 1
    //     virtual:bench      a loopback bus, no hardware
    std::string device;

    uint32_t bitrateBps { 500000 };
    // Zero means classic CAN. Anything else asks for CAN FD, and the adapter
    // has to support it.
    uint32_t dataBitrateBps { 0 };

    // Zero takes the CiA default for the rate, which is where every other node
    // on the bus will be sampling.
    uint16_t samplePointPermille { 0 };
    uint16_t dataSamplePointPermille { 0 };

    // Receive but never transmit: no acknowledgements, no error frames. The
    // safe way to attach to a bus that is not yours, and the right default for
    // anything being observed rather than driven.
    bool listenOnly { false };

    // Where frames go and come from.
    std::string rxKey;
    std::string txKey;

    // How many frames may back up before the oldest are dropped. A busy
    // 500 kbit/s bus is about 4000 frames a second, so the default holds a
    // couple of seconds of one.
    uint32_t rxQueueDepth { 8192 };

    // Publish received frames at all. Off for a channel that exists only to
    // transmit, where publishing would echo a diagnostic tool's own traffic
    // back at it.
    bool publishRx { true };
    // Accept frames from `txKey` and put them on the bus. Off is a stronger
    // statement than listenOnly: nothing can even ask.
    bool acceptTx { true };
};

struct NodeConfig
{
    std::vector<ChannelConfig> channels;

    // Where the bridge's own topics live.
    std::string statusKey { "vehicle/can/status" };
    std::string setBitrateKey { "vehicle/can/set_bitrate" };

    // How often the status topic is republished even when nothing changed, so
    // a late subscriber does not have to wait for an event.
    uint32_t statusIntervalMs { 1000 };

    // Take a PCAN adapter away from the kernel driver holding it. Linux only,
    // and it removes the socketcan interface that driver created -- which is
    // why it is a config key rather than something inferred.
    bool pcanDetachKernelDriver { false };

    // Carry on when a channel cannot be opened. On by default: with two buses
    // configured, one unplugged adapter should not take the other down.
    bool continueOnChannelError { true };
};

// Reads the file. Returns false and logs every problem it found rather than
// the first, so a config with three typos takes one run to fix.
bool load_node_config(const std::string& path, NodeConfig& out);

// Exposed for tests.
bool parse_node_config(const std::string& yaml, NodeConfig& out);

} // namespace can_bridge

#endif // CAN_BRIDGE_NODE_CONFIG_H
