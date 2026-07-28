// SPDX-License-Identifier: GPL-3.0-or-later
// Non-Linux stand-in for the usbfs transport.
//
// The real implementation (usb_device.cpp) is raw usbfs ioctls against
// /dev/bus/usb and has no equivalent outside Linux. This exists so the portable
// half of apple_usb -- the mux state machine, the usbmuxd socket server, the
// lockdown client -- still compiles and links on a developer's macOS box, which
// is where its unit tests run.
//
// Every entry point fails rather than pretending: enumeration finds nothing,
// and the transfer wrappers throw the same std::system_error type the real ones
// do, with ENOSYS. A test that reaches one of these has stopped testing
// protocol logic and started testing hardware I/O, and should be told so
// loudly rather than silently passing against a no-op.
#include "apple_usb/usb_device.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <system_error>

namespace apple_usb
{
namespace
{

[[noreturn]] void unsupported(const char* what)
{
    SPDLOG_ERROR("[usb] {} is Linux-only; this build has no USB transport", what);
    throw std::system_error(ENOSYS, std::generic_category(), what);
}

}  // namespace

std::vector<DeviceInfo> listAppleDevices()
{
    // Not fatal: callers poll this, and "no phone attached" is a legitimate
    // answer that keeps a node running instead of crashing it at startup.
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        SPDLOG_WARN("[usb] device enumeration is Linux-only; no devices will ever be found");
    }
    return {};
}

std::optional<int> openDevice(const DeviceInfo& /*device*/)
{
    return std::nullopt;
}

bool switchToCarPlayConfiguration(const DeviceInfo& /*device*/)
{
    return false;
}

std::vector<unsigned int> boundInterfaces(const DeviceInfo& /*device*/)
{
    return {};
}

bool usbDisconnectKernelDriver(int /*fd*/, unsigned int /*iface*/)
{
    return false;
}

bool usbClearHalt(int /*fd*/, uint8_t /*endpoint*/)
{
    return false;
}

void usbClaimInterface(int /*fd*/, unsigned int /*iface*/)
{
    unsupported("usbClaimInterface");
}

std::vector<uint8_t> usbControl(int /*fd*/, uint8_t /*bmRequestType*/, uint8_t /*bRequest*/,
                                uint16_t /*wValue*/, uint16_t /*wIndex*/, uint16_t /*wLength*/,
                                const uint8_t* /*out_data*/, unsigned /*timeout_ms*/)
{
    unsupported("usbControl");
}

void usbBulkOut(int /*fd*/, uint8_t /*endpoint*/, const uint8_t* /*data*/, size_t /*len*/,
                unsigned /*timeout_ms*/)
{
    unsupported("usbBulkOut");
}

std::vector<uint8_t> usbBulkIn(int /*fd*/, uint8_t /*endpoint*/, size_t /*max_len*/,
                               unsigned /*timeout_ms*/)
{
    unsupported("usbBulkIn");
}

}  // namespace apple_usb
