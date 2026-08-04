// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCAN-USB FD adapters over libusb, without PEAK's driver or their PCBUSB
// library.
//
// How multi-channel is handled, which is the question this backend exists to
// answer:
//
//   A PCAN-USB Pro FD is one USB device with one pair of bulk endpoints
//   carrying both CAN channels' traffic interleaved, tagged by a channel index
//   in every record. It cannot be opened twice. So the backend keeps one
//   PcanDevice per USB interface, reference-counted by the channels handed out
//   of it: opening `pcan:0/0` creates the device, opening `pcan:0/1` finds the
//   existing one, and the device closes when the last channel does. A single
//   reader thread per device demultiplexes incoming records into per-channel
//   queues.
//
//   Nothing above this file sees any of that. `pcan:0/1` is a channel id like
//   any other, and the config, the logs and the command line all name it the
//   same way they name `socketcan:can0`.
//
// The PCAN-USB X6 is six channels behind three USB interfaces of two channels
// each, so channel 4 is interface 2's local channel 0. That mapping is applied
// here and is the part of this backend most likely to need adjusting against
// real hardware -- see the note on provenance in pucan.h.
//
// Platform: libusb works on both macOS and Linux, so this backend builds and
// runs on both. On Linux, though, a PCAN adapter is claimed by the in-tree
// `peak_usb` driver as soon as it is plugged in, and taking it away from that
// driver destroys the socketcan interface it created. So the default is to
// refuse a device the kernel holds and say so, and detaching is opt-in. On
// Linux the SocketCAN backend is the better path anyway.
#ifndef CAN_PCAN_BACKEND_H
#define CAN_PCAN_BACKEND_H

#include "can/backend.h"

#include <memory>

namespace can::pcan
{

struct PcanOptions
{
    // Take the device away from the kernel driver that holds it, if any. Only
    // meaningful on Linux, where it removes the socketcan interface the
    // peak_usb driver created for the adapter. Off by default: silently
    // dismantling an interface something else may be using is not a thing to
    // do without being asked.
    bool detachKernelDriver { false };

    // How long a bulk read waits before giving the reader thread a chance to
    // notice it should stop. Not a bus timeout -- a quiet bus produces no
    // transfers at all, which is normal.
    unsigned int readTimeoutMs { 100 };
    unsigned int writeTimeoutMs { 1000 };
};

std::shared_ptr<Backend> make_pcan_backend(const PcanOptions& options = {});

} // namespace can::pcan

#endif // CAN_PCAN_BACKEND_H
