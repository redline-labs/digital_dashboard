// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which CAN channel, written as one string.
//
//     socketcan:can0        the kernel interface named can0
//     pcan:0                the first PCAN dongle found, its channel 0
//     pcan:0/1              that same dongle, channel 1
//     pcan:LSN00123/1       the dongle with that serial, channel 1
//     virtual:a             a loopback channel, no hardware
//
// This is the piece that answers "how do we handle a backend with several
// channels per device alongside one with a channel per device". A device is a
// backend-private concept: SocketCAN's `can0` is a whole channel on its own,
// while a PCAN-USB Pro FD is one USB handle serving two. Both come out as a
// flat list of channel ids, so a config file, a log line and a command-line
// argument all name a channel the same way, and only the PCAN backend has to
// know that two of its ids share one handle.
#ifndef CAN_CHANNEL_ID_H
#define CAN_CHANNEL_ID_H

#include "can/error.h"

#include <cstdint>
#include <string>

namespace can
{

struct ChannelId
{
    // "socketcan", "pcan", "virtual". Lower case.
    std::string backend;
    // Whatever identifies the device within that backend: an interface name,
    // an index, a serial number. Opaque here on purpose.
    std::string device;
    // Which channel on that device. Always 0 for a backend whose devices have
    // exactly one.
    uint8_t channel { 0 };

    bool operator==(const ChannelId& other) const = default;
    // So a ChannelId can key a std::map.
    bool operator<(const ChannelId& other) const;

    // Round-trips through parse_channel_id(). The channel suffix is omitted
    // when it is 0, so "pcan:0" and "pcan:0/0" are the same channel and the
    // shorter form is what gets printed.
    std::string toString() const;
};

Result<ChannelId> parse_channel_id(const std::string& text);

// What a backend found. `id` is enough to open it; the rest is for a human
// choosing between them.
struct ChannelInfo
{
    ChannelId id;
    // "PCAN-USB Pro FD", "can0".
    std::string description;
    // Empty when the backend cannot cheaply determine it.
    std::string serial;
    bool supportsFd { false };
    // False when the channel was found but cannot currently be opened -- a
    // PCAN dongle held by the kernel driver, say. `unavailableReason` says why.
    bool available { true };
    std::string unavailableReason;
};

} // namespace can

#endif // CAN_CHANNEL_ID_H
