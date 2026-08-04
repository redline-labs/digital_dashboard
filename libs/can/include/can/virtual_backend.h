// SPDX-License-Identifier: GPL-3.0-or-later
//
// A CAN bus made of nothing.
//
// `virtual:<bus>` opens a channel onto a named in-process bus. Every channel
// sharing a name sees every other channel's traffic, which is what a real bus
// does, so two of them make a working pair with no hardware in between.
//
// This exists for the same reason the CANopen library has a simulated keypad:
// the bridge node's real work is queueing, threading, topic mapping and
// lifecycle, and none of that needs a dongle to be wrong. Being able to run the
// node for real -- publishing frames a subscriber can see -- is the difference
// between testing those parts and hoping about them.
//
// A frame is not delivered back to the channel that sent it, matching a CAN
// controller without loopback enabled. Use virtual_bus_inject() to play the
// part of some other node on the bus.
#ifndef CAN_VIRTUAL_BACKEND_H
#define CAN_VIRTUAL_BACKEND_H

#include "can/backend.h"

#include "helpers/can_frame.h"

#include <memory>
#include <string>

namespace can
{

std::shared_ptr<Backend> make_virtual_backend();

// Puts a frame on a virtual bus as though another node had sent it. Every open
// channel on that bus receives it. Does nothing if no channel is open on the
// named bus, which is the same thing that happens when you transmit onto a bus
// nobody is listening to.
void virtual_bus_inject(const std::string& bus, const helpers::CanFrame& frame);

// How many channels are currently open on a bus. For tests that want to know
// the node has finished starting up before they inject.
size_t virtual_bus_channel_count(const std::string& bus);

} // namespace can

#endif // CAN_VIRTUAL_BACKEND_H
