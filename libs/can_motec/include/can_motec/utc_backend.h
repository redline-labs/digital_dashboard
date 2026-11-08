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
// The bit rate goes in the Mode command every session sends (motec_gw.h,
// make_mode), so it is set at open and set_bitrate() starts a new session to
// change it. Six rates only: 1 Mbit/s, 500, 250, 125, 66.67 and 33.33 kbit/s;
// anything else, and any sample point, is refused or ignored respectively --
// the protocol has no field for either. Only 1 Mbit/s (index 0) has been seen
// working on hardware; the other indices are CAN Inspector's table.
//
// WHAT THIS BACKEND CANNOT DO, because the protocol behind it is not published
// and these parts of it were never worked out:
//
//   Read the bit rate back. bitrate() is what this session told the device.
//
//   Keep up with a saturated bus. A UTC passes about 8,100 frames a second at
//   1 Mbit/s (a full bus of 8-byte 11-bit frames is about 8,400); past that it
//   buffers about 2,000, then loses about three in every 131 and damages one
//   more, which is dropped and counted in rxDropped. It transmits about 570
//   frames a second, however fast send() is called.
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
// See docs/libs/can_motec.md for the protocol, its provenance, and what would be
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

    // How long to wait for the device to answer Open, Version and Mode
    // during bring-up.
    unsigned int handshakeTimeoutMs { 1000 };

    // How long send() waits for the device to answer an earlier transmit when
    // two are already outstanding (utc_backend.cpp, kTxWindow), before
    // deciding it has stopped answering.
    unsigned int txAnswerTimeoutMs { 1000 };

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
