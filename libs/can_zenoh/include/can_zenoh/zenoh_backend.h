// SPDX-License-Identifier: GPL-3.0-or-later
//
// `zenoh:<prefix>` -- the far end of a bridged bus, as a can::Channel.
//
// can_bridge (or a simulator) puts a bus on `<prefix>/rx` and takes frames to
// transmit from `<prefix>/tx`. This channel is the other side of that: receive()
// returns what arrives on `<prefix>/rx`, and send() publishes on `<prefix>/tx`.
// So a tool written against can::Channel talks to an adapter on another machine,
// or to a simulated device, exactly as it would to one plugged in here:
//
//     zenoh:vehicle/can0     the bus can_bridge calls can0, wherever it runs
//
// What a remote bus cannot do, this cannot either: there is no bit rate to set
// (the bridge owns the hardware) and no bus state beyond the frames, so
// set_bitrate() is Unsupported and statistics() counts only what passed here.
// As on real hardware, a frame sent is not received back: the bridge publishes
// only what the adapter received.
//
// The prefix is a zenoh key and is checked as one. Its last segment must not
// be all digits, because a channel id reads a trailing `/N` as a channel number.
#ifndef CAN_ZENOH_ZENOH_BACKEND_H_
#define CAN_ZENOH_ZENOH_BACKEND_H_

#include "can/backend.h"

#include <memory>

namespace can_zenoh
{

std::shared_ptr<can::Backend> make_zenoh_backend();

}  // namespace can_zenoh

#endif  // CAN_ZENOH_ZENOH_BACKEND_H_
