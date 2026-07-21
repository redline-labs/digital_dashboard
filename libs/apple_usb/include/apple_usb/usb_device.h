// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
#ifndef APPLE_USB_USB_DEVICE_H_
#define APPLE_USB_USB_DEVICE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apple_usb
{

inline constexpr uint16_t kAppleVendorId = 0x05ac;

// Apple's "CarPlay" USB configuration; the default configuration is 4.
inline constexpr uint8_t kCarPlayConfiguration = 6;

struct DeviceInfo
{
    std::string sysfs_path;   // e.g. /sys/bus/usb/devices/1-1
    std::string usbfs_path;   // e.g. /dev/bus/usb/001/004
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string serial;       // 24/25-char UDID as reported by the device
    uint8_t active_configuration = 0;
};

// Scan /sys/bus/usb/devices for connected Apple devices.
std::vector<DeviceInfo> listAppleDevices();

// Open the usbfs node for a device. Returns the fd or nullopt.
std::optional<int> openDevice(const DeviceInfo& device);

// Issue the Apple vendor request (bmRequestType 0xC0, bRequest 0x52) and
// switch the device to the CarPlay configuration (bConfigurationValue 6).
// The switch itself is applied through sysfs (bConfigurationValue) because
// re-enumeration invalidates any open usbfs fd. Returns true on success.
bool switchToCarPlayConfiguration(const DeviceInfo& device);

// --- Low-level usbfs wrappers (Linux). Throw std::system_error on failure. ---

// Claim an interface (USBDEVFS_CLAIMINTERFACE).
void usbClaimInterface(int fd, unsigned int iface);

// Synchronous control transfer (USBDEVFS_CONTROL). Returns bytes transferred.
std::vector<uint8_t> usbControl(int fd, uint8_t bmRequestType, uint8_t bRequest,
                                uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                const uint8_t* out_data = nullptr, unsigned timeout_ms = 3000);

// Synchronous bulk OUT transfer (USBDEVFS_BULK).
void usbBulkOut(int fd, uint8_t endpoint, const uint8_t* data, size_t len, unsigned timeout_ms = 2000);

// Synchronous bulk IN transfer (USBDEVFS_BULK). Returns bytes read; errno
// ETIMEDOUT surfaces as a std::system_error the caller can distinguish.
std::vector<uint8_t> usbBulkIn(int fd, uint8_t endpoint, size_t max_len, unsigned timeout_ms = 2000);

}  // namespace apple_usb

#endif  // APPLE_USB_USB_DEVICE_H_
