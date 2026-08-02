// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
#ifndef APPLE_USB_USB_DEVICE_H_
#define APPLE_USB_USB_DEVICE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// libusb's handle type, forward declared so this header stays free of
// <libusb.h>. Everything the rest of the library needs is expressed in the
// plain structs below, which is what lets the protocol layer build anywhere.
struct libusb_device_handle;

namespace apple_usb
{

inline constexpr uint16_t kAppleVendorId = 0x05ac;

// Apple's "CarPlay" USB configuration; the default configuration is 4.
inline constexpr uint8_t kCarPlayConfiguration = 6;

// libusb's own ceiling on how deep a device can sit in the hub tree.
inline constexpr size_t kMaxPortPathDepth = 7;

// Where a device physically sits: the bus, then the chain of hub port numbers
// from the root hub down to it.
//
// This -- not the serial number -- is how a phone is tracked across the
// CarPlay configuration switch. The Apple vendor request re-enumerates the
// device, which changes its address and its usbfs node but *not* which socket
// it is plugged into, so the port path survives while everything else moves.
// Reading it also needs no open handle, whereas the serial number lives in a
// string descriptor that costs a device open and a control transfer.
struct PortPath
{
    uint8_t bus = 0;
    std::vector<uint8_t> ports;

    bool operator==(const PortPath& other) const = default;

    bool empty() const { return ports.empty(); }

    // "1-4.2", matching how the kernel names the device in sysfs -- so this
    // doubles as the sysfs directory name on Linux.
    std::string toString() const;
};

struct DeviceInfo
{
    PortPath port;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t address = 0;
    uint8_t active_configuration = 0;
    uint8_t num_configurations = 0;

    // The 24/25-char UDID. Enumeration deliberately leaves this empty: it is a
    // string descriptor, so filling it in would mean opening every device on
    // every poll. Call readSerial() once, after you hold a handle.
    std::string serial;
};

// --- Portable descriptor model --------------------------------------------
//
// Enough of the configuration descriptor for the mux and NCM layers to find
// their interfaces and endpoints without touching sysfs or libusb directly.

enum class TransferType : uint8_t
{
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3,
};

struct EndpointInfo
{
    uint8_t address = 0;
    uint8_t attributes = 0;
    uint16_t max_packet_size = 0;

    TransferType type() const { return static_cast<TransferType>(attributes & 0x03); }
    bool isIn() const { return (address & 0x80) != 0; }
};

struct InterfaceInfo
{
    uint8_t number = 0;
    uint8_t alt_setting = 0;
    uint8_t iface_class = 0;
    uint8_t subclass = 0;
    uint8_t protocol = 0;
    std::vector<EndpointInfo> endpoints;

    // Class-specific descriptors that follow this interface descriptor -- for
    // a CDC control interface, the functional descriptors. libusb has already
    // scoped these to the interface, which is the whole reason the old
    // hand-rolled walk over /sys/.../descriptors could go away.
    std::vector<uint8_t> extra;
};

struct ConfigInfo
{
    uint8_t value = 0;
    // Every alt setting of every interface, flattened. Iterate and filter.
    std::vector<InterfaceInfo> interfaces;
};

// An open handle to a device. Move-only; closes on destruction.
class DeviceHandle
{
  public:
    DeviceHandle() = default;
    explicit DeviceHandle(libusb_device_handle* handle) : handle_(handle) {}
    ~DeviceHandle();

    DeviceHandle(DeviceHandle&& other) noexcept;
    DeviceHandle& operator=(DeviceHandle&& other) noexcept;
    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;

    explicit operator bool() const { return handle_ != nullptr; }
    void reset();

    // For use inside this library only; callers should prefer the wrappers.
    libusb_device_handle* native() const { return handle_; }

  private:
    libusb_device_handle* handle_ = nullptr;
};

// --- Enumeration -----------------------------------------------------------

// Every connected Apple device. Opens nothing, so `serial` comes back empty.
std::vector<DeviceInfo> listAppleDevices();

// Re-read one device by its physical location. Returns nullopt if nothing is
// plugged in there any more. This is the lookup used after a re-enumeration.
std::optional<DeviceInfo> findDeviceAt(const PortPath& port);

// Open a device by its physical location. Returns an empty handle on failure.
DeviceHandle openDevice(const DeviceInfo& device);
DeviceHandle openDevice(const PortPath& port);

// Read the UDID from the device's iSerialNumber string descriptor. Needs an
// open handle but no claimed interface -- it is a control transfer on
// endpoint 0, so it works even while kernel drivers hold the interfaces.
std::string readSerial(const DeviceHandle& handle);

// Read an arbitrary string descriptor by index. Empty if it cannot be read.
//
// The one that matters here is iMACAddress, named by the CDC Ethernet
// functional descriptor: it is how the host MAC of an NCM function is found,
// and on macOS -- where the kernel drives NCM and hands us a ready-made network
// interface -- it is the only reliable way to tell which interface belongs to
// which NCM function. Same endpoint-0 control transfer as readSerial(), so it
// works while drivers hold the interfaces, which is exactly the situation.
std::string readStringDescriptor(const DeviceHandle& handle, uint8_t index);

// The active configuration's descriptor, or nullopt if it cannot be read.
std::optional<ConfigInfo> readActiveConfig(const DeviceInfo& device);
std::optional<ConfigInfo> readActiveConfig(const PortPath& port);

// --- Configuration and driver arbitration ----------------------------------

// Issue the Apple vendor request (bmRequestType 0xC0, bRequest 0x52) to unlock
// the CarPlay configurations, then select configuration 6.
//
// The vendor request re-enumerates the device, so this waits for it to come
// back at the same port path before selecting. Returns true on success; the
// caller must re-read the DeviceInfo afterwards (see findDeviceAt) because the
// address will have changed.
bool switchToCarPlayConfiguration(const DeviceInfo& device);

// Whether this process is able to take a device away from the drivers that
// already own it, which everything from the configuration switch onwards needs.
//
// Linux gets that from the udev rules and this is always true there. macOS only
// grants it to root (the alternative, the com.apple.vm.device-access
// entitlement, is issued to virtualization vendors), so on macOS the node has
// to run under sudo. On failure `why_not` is filled in with something worth
// showing a user.
//
// Checking is cheap and touches no device, so callers can use it as a preflight
// rather than discovering the problem several layers down as a bare
// LIBUSB_ERROR_ACCESS.
bool canDetachDevices(std::string& why_not);

// True when a kernel driver currently holds this interface. Always false on
// platforms with no concept of one.
bool kernelDriverActive(const DeviceHandle& handle, uint8_t iface);

// Ask the kernel to release an interface from whatever driver holds it.
// Needed before changing the configuration -- the kernel refuses while any
// interface is claimed -- and before claiming an interface that ipheth or
// cdc_ncm has taken. Best-effort: returns false if it could not be done.
//
// Deliberately not libusb_set_auto_detach_kernel_driver: that only fires on
// claim, and the configuration switch needs interfaces released *without*
// claiming them.
bool detachKernelDriver(const DeviceHandle& handle, uint8_t iface);

// Release every interface of `config` that has a driver bound. Returns how
// many were detached.
unsigned detachKernelDrivers(const DeviceHandle& handle, const ConfigInfo& config);

// --- Transfers. Throw std::system_error on failure. ------------------------
//
// libusb error codes are mapped onto the errno values the old usbfs wrappers
// produced, so callers can keep distinguishing ETIMEDOUT (a poll that found
// nothing) from ENODEV (the phone went away).

void usbClaimInterface(const DeviceHandle& handle, uint8_t iface);
void usbReleaseInterface(const DeviceHandle& handle, uint8_t iface);

// Select an alternate setting. The NCM data interface only exposes its bulk
// endpoints on altsetting 1.
void usbSetAltSetting(const DeviceHandle& handle, uint8_t iface, uint8_t alt_setting);

// Synchronous control transfer. Returns the bytes read (device-to-host) or an
// empty buffer (host-to-device).
std::vector<uint8_t> usbControl(const DeviceHandle& handle, uint8_t bmRequestType,
                                uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                uint16_t wLength, const uint8_t* out_data = nullptr,
                                unsigned timeout_ms = 3000);

// Synchronous bulk OUT transfer.
void usbBulkOut(const DeviceHandle& handle, uint8_t endpoint, const uint8_t* data, size_t len,
                unsigned timeout_ms = 2000);

// Synchronous bulk IN transfer. A timeout that still moved data returns that
// data rather than throwing; a timeout that moved none throws ETIMEDOUT.
std::vector<uint8_t> usbBulkIn(const DeviceHandle& handle, uint8_t endpoint, size_t max_len,
                               unsigned timeout_ms = 2000);

// Reset an endpoint's halt condition and, importantly, its data toggle.
// Required after taking an endpoint over from a kernel driver: if the device's
// toggle and ours disagree it treats everything we send as a duplicate and
// NAKs forever, which surfaces as bulk writes timing out while reads on the
// same interface keep working.
bool usbClearHalt(const DeviceHandle& handle, uint8_t endpoint);

}  // namespace apple_usb

#endif  // APPLE_USB_USB_DEVICE_H_
