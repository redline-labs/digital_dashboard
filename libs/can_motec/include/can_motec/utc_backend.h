// SPDX-License-Identifier: GPL-3.0-or-later
//
// A MoTeC UTC as a can::Channel.
//
// Channel ids:
//
//     motec:0                  the first UTC attached
//     motec:56536              the UTC with that serial
//     motec:udp=192.168.1.40   a network gateway, default port 29456
//     motec:udp=[fe80::1]:4000 the same, with an explicit port
//
// WHAT THIS BACKEND CANNOT DO, because the protocol behind it is not published
// and these parts of it were never worked out:
//
//   Set a bit rate. The `Set` command exists and its argument is known to be
//   shaped like 0x40000000 | (value << 18), but which register selects the bus
//   speed is not. The device runs at whatever MoTeC's own tool last configured
//   it for, and set_bitrate() therefore FAILS rather than pretending. It also
//   cannot read the rate back, so bitrate() returns what the caller asked for
//   -- flagged here because that is exactly the "reports intent, not reality"
//   trap the SocketCAN backend was fixed for. The difference is that there the
//   truth was available and simply not asked for; here there is no way to ask.
//
//   Listen only. There is no known command, and a receive filter is not the
//   same thing -- it stops frames arriving, not this node acknowledging them.
//   An open that asks for it fails rather than silently transmitting.
//
//   CAN FD. The hardware is classic CAN: eight bytes, one bit rate.
//
//   Report bus state or error counters. Nothing in the protocol carries them,
//   so statistics() reports traffic and drops but leaves the controller state
//   Unknown while the link is alive.
//
//   Send remote frames. The flags byte has four bits whose meaning is not
//   known and one of them is presumably RTR; guessing would put a data frame
//   on the bus where a request was meant.
//
// See docs/motec_utc.md for the protocol, its provenance, and what would be
// needed to close each gap.
#ifndef CAN_MOTEC_UTC_BACKEND_H
#define CAN_MOTEC_UTC_BACKEND_H

#include "can/backend.h"

#include <memory>

namespace can::motec
{

struct MotecOptions
{
    // How long a bulk read waits before giving the reader thread a chance to
    // notice it should stop. Not a bus timeout: a quiet bus still produces a
    // keep-alive roughly every 255 ms.
    unsigned int readTimeoutMs { 100 };
    unsigned int writeTimeoutMs { 1000 };

    // How long the device may go silent before the channel is treated as
    // stalled. Generously above the ~255 ms push period, because missing one
    // keep-alive means nothing and missing twelve means the link is gone.
    unsigned int rxStallTimeoutMs { 3000 };

    // Only meaningful if something has bound a driver to the dongle, which
    // nothing in mainline Linux does -- ftdi_sio ignores MoTeC's product id
    // unless it is told about it with a new_id write.
    bool detachKernelDriver { false };

    // How long to wait for the device to answer Open, Version and Filter
    // during bring-up.
    unsigned int handshakeTimeoutMs { 1000 };

    // How often to send a Version command purely to keep the session open.
    //
    // NOT optional, and not a nicety. A real UTC closes the Rx stream about
    // ten seconds after the subscribe unless the client keeps talking to it:
    // the data frames stop, the idle keep-alives stop, and the device answers
    // a fresh subscribe with status 0x04. Nothing is reported before it
    // happens, so a bridge simply goes deaf ten seconds after it starts --
    // which looks exactly like a bus that went quiet.
    //
    // Two seconds is comfortably inside the window and costs one eight-byte
    // command. `Version` is the command the reference client uses for this;
    // `Ack` is NOT -- sending one stops the stream immediately.
    unsigned int keepAliveIntervalMs { 2000 };
};

std::shared_ptr<Backend> make_motec_backend(const MotecOptions& options = {});

} // namespace can::motec

#endif // CAN_MOTEC_UTC_BACKEND_H
