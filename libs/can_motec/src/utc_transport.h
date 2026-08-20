// SPDX-License-Identifier: GPL-3.0-or-later
//
// A frame pipe to a MoTeC gateway.
//
// The command envelope is the same whether it arrives over the UTC dongle's
// bulk endpoints or over UDP from one of MoTeC's network gateways, so the
// session logic above this file is written once and neither half knows which
// it has. What differs is entirely below:
//
//   USB   a byte stream. Frames straddle 64-byte packets, every inbound packet
//         carries two FTDI status bytes that are not part of the stream, and a
//         dropped byte has to be recovered from. The FrameReader lives here.
//
//   UDP   one datagram is exactly one frame. No reassembly, no resynchronising,
//         and a lost datagram is simply a lost frame.
//
// The UDP path is not incidental: it is the only one that can be exercised
// without a dongle, and it is what the session sequence in utc_backend.cpp was
// developed against. See docs/motec_utc.md.
#ifndef CAN_MOTEC_UTC_TRANSPORT_H
#define CAN_MOTEC_UTC_TRANSPORT_H

#include "can_motec/motec_gw.h"

#include "can/channel.h"
#include "can/error.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace can::motec
{

class Transport
{
public:
    virtual ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // "MoTeC UTC serial 56536", "MoTeC gateway at [fe80::1]:29456".
    virtual const std::string& description() const = 0;

    // Sends one complete frame. Safe to call from a thread other than the one
    // in receive(); implementations serialise writes among themselves.
    virtual Result<void> send(std::span<const uint8_t> bytes) = 0;

    // Appends whatever frames have arrived, waiting up to `timeout` for the
    // first. Returning zero frames on timeout is not an error -- the device
    // pushes a keep-alive roughly every 255 ms, but nothing guarantees one
    // lands inside any particular window.
    virtual Result<size_t> receive(std::vector<Frame>& out, Duration timeout) = 0;

    // Bytes discarded resynchronising, for the stream transports. Always zero
    // for a datagram one.
    virtual uint64_t resyncBytes() const { return 0; }

protected:
    Transport() = default;
};

struct UsbTarget
{
    // Tried in order: the index first when `byIndex` is set, then the serial.
    //
    // Both, rather than one or the other, because a UTC's serial is a bare
    // number -- the one on this desk is 56536 -- so "is this text an index or
    // a serial" has no answer. Treating a digit string as an index only would
    // make every real serial unusable; treating it as a serial only would
    // break `motec:0`. So a digit string means "index if there is one, and
    // otherwise a serial", and `serial=` forces the second reading.
    bool byIndex { false };
    unsigned int index { 0 };
    std::string serial;
};

// Opens the dongle. `detachKernelDriver` matters only if something has bound a
// driver to it -- nothing in mainline Linux does, since ftdi_sio does not
// claim MoTeC's product id without a new_id write.
Result<std::shared_ptr<Transport>> open_usb_transport(const UsbTarget& target,
                                                      unsigned int readTimeoutMs,
                                                      unsigned int writeTimeoutMs,
                                                      bool detachKernelDriver);

// Everything attached, for enumerate(). Never fails: no libusb, no devices.
struct UsbDeviceInfo
{
    unsigned int index { 0 };
    std::string serial;
    bool available { true };
    std::string unavailableReason;
};

std::vector<UsbDeviceInfo> list_usb_devices();

// host may be a bare address or a name; port 0 takes MoTeC's default of 29456.
Result<std::shared_ptr<Transport>> open_udp_transport(const std::string& host, uint16_t port);

inline constexpr uint16_t kDefaultGatewayPort = 29456;

} // namespace can::motec

#endif // CAN_MOTEC_UTC_TRANSPORT_H
