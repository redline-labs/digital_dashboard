// SPDX-License-Identifier: GPL-3.0-or-later
//
// The usbmuxd control protocol's framing: a 16-byte little-endian header
// followed by an XML property list.
//
// Split from the socket because the length field is the one number in this
// stack that an unprivileged local client fully controls, and it is used
// directly as an allocation size. Anything connecting to the usbmuxd socket can
// send it -- our own node, libimobiledevice tools, or whatever else a developer
// runs -- so the bounds are checked before the value is believed, and that
// belongs somewhere it can be tested against byte strings.
#ifndef APPLE_USB_USBMUXD_FRAMING_H_
#define APPLE_USB_USBMUXD_FRAMING_H_

#include "plist/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apple_usb::usbmuxd
{

constexpr size_t kHeaderSize = 16;
constexpr uint32_t kPlistVersion = 1;
constexpr uint32_t kPlistMessage = 8;

// A ceiling on a control message. The largest thing a client legitimately sends
// is a SavePairRecord carrying a few kilobytes of PEM.
constexpr uint32_t kMaxRequestBytes = 1u << 20;

struct Header
{
    uint32_t length = 0;  // total, including this header
    uint32_t version = 0;
    uint32_t message = 0;
    uint32_t tag = 0;     // echoed back on the reply

    // Bytes of body following the header.
    size_t bodySize() const { return length - kHeaderSize; }
};

// Parses `kHeaderSize` bytes. Returns nullopt when the declared length is
// impossible -- below the header itself, which would underflow the body size,
// or above the ceiling, which would be an unbounded allocation.
std::optional<Header> parseHeader(const uint8_t* data);

// Frames one reply: the header, then the plist as XML.
std::vector<uint8_t> encodeReply(uint32_t tag, const plist::Value& dict);

// The standard Result reply.
plist::Value resultDict(int number);

// A string-valued key, or empty when absent or not a string.
std::string dictString(const plist::Value& dict, const char* key);

// The dashed spelling of an undashed 24-character serial, and vice versa.
// Empty when the input is neither form.
//
// Modern iPhones report a 24-character serial that libusbmuxd and the phone's
// own lockdown service disagree about the spelling of; a pair record filed
// under one form is invisible under the other. This cost a real hardware
// session -- see docs/carplay_bringup.md stage 4.
std::string alternateUdidForm(const std::string& udid);

}  // namespace apple_usb::usbmuxd

#endif  // APPLE_USB_USBMUXD_FRAMING_H_
