// SPDX-License-Identifier: GPL-3.0-or-later
//
// When a usbmux write needs a zero-length packet after it. Without one, a mux
// packet whose size is an exact multiple of the endpoint's packet size never
// ends as far as the phone is concerned: it waits for the rest, and the stream
// stalls until the next write happens to close it. A stray ZLP is no better --
// the phone reads it as an empty transfer -- so the cases that must NOT get one
// matter as much as the ones that must.
#include "apple_usb/usb_device.h"

#include <spdlog/spdlog.h>

#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

}  // namespace

int main()
{
    using apple_usb::needsZeroLengthPacket;

    // High speed: 512-byte bulk packets.
    expect(needsZeroLengthPacket(512, 512), "one full packet needs a ZLP");
    expect(needsZeroLengthPacket(1024, 512), "two full packets need a ZLP");
    expect(!needsZeroLengthPacket(476, 512), "a short packet ends the transfer itself");
    expect(!needsZeroLengthPacket(513, 512), "a short last packet ends the transfer itself");
    expect(!needsZeroLengthPacket(16, 512), "a bare mux header ends the transfer itself");

    // SuperSpeed: 1024-byte packets. 1536 is a multiple of 512 but ends short here.
    expect(!needsZeroLengthPacket(1536, 1024), "1536 at 1024 is short, no ZLP");
    expect(needsZeroLengthPacket(2048, 1024), "2048 at 1024 needs a ZLP");

    // Degenerate inputs.
    expect(!needsZeroLengthPacket(0, 512), "an empty write is already a ZLP");
    expect(!needsZeroLengthPacket(512, 0), "an unknown packet size sends no ZLP");

    if (failures == 0)
    {
        SPDLOG_INFO("all zero-length-packet checks passed");
    }
    return failures == 0 ? 0 : 1;
}
