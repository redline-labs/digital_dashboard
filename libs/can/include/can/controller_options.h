// SPDX-License-Identifier: GPL-3.0-or-later
//
// Controller behaviours beyond bit rate and listen-only, which only some
// adapters offer.
//
// Every field defaults to what a CAN controller does when nobody asks, so the
// all-default set is accepted by every channel and anything else is opt-in per
// backend: a channel that cannot do one answers Unsupported naming it, rather
// than accepting a request it will not carry out.
#ifndef CAN_CONTROLLER_OPTIONS_H
#define CAN_CONTROLLER_OPTIONS_H

#include <optional>
#include <string>

namespace can
{

struct ControllerOptions
{
    // Acknowledge our own frames as well as everyone else's. A lone node on a
    // bench then sees its transmissions succeed instead of retrying forever on
    // a missing ACK. Outside CAN's rules -- a transmitter never acks -- and it
    // hides exactly the fault an absent receiver would otherwise show, so it
    // has no place on a bus that has other nodes.
    bool selfAcknowledge { false };

    // Receive as a classic CAN 2.0 controller: a recessive r0 does not switch
    // to FD, and an FD frame is a format error. For a bus with a misbehaving
    // non-FD node; on a bus carrying FD traffic, this node will flag every FD
    // frame with an error frame.
    bool classicFramesOnly { false };

    // Treat a frame with BRS set as one to sit out: wait for bus idle rather
    // than follow it into the data phase.
    bool ignoreBitrateSwitchFrames { false };

    // Stamp a received frame at its start rather than at its end.
    bool timestampAtStartOfFrame { false };

    // Hand each frame to the host as soon as it arrives instead of batching
    // to fill a transfer. Lower latency, more transfers.
    bool lowLatencyReceive { false };

    bool operator==(const ControllerOptions&) const = default;
};

// The name of the first option `requested` sets that `supported` does not, or
// nothing when all of them are supported.
std::optional<std::string> first_unsupported(const ControllerOptions& requested,
                                             const ControllerOptions& supported);

std::string to_string(const ControllerOptions& options);

} // namespace can

#endif // CAN_CONTROLLER_OPTIONS_H
