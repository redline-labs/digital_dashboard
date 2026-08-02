// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
//
// Backed by libusb rather than raw usbfs ioctls. That is not just tidiness:
// libusb parses configuration descriptors for us, which removed three separate
// hand-rolled sysfs walks (interface classes, endpoint addresses, and the CDC
// functional descriptors), and it works on every platform we build for, so the
// USB half of this library is no longer Linux-only.
#include "apple_usb/usb_device.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef __APPLE__
#include <unistd.h>
#endif

namespace apple_usb
{

namespace
{

// How this host takes a device away from whatever already owns it.
//
// Linux detaches one interface at a time and the driver comes back when we let
// go. macOS has no such thing: it *captures the whole device* at once
// (kUSBReEnumerateCaptureDeviceMask), and it needs either root or the
// com.apple.vm.device-access entitlement, which Apple grants to virtualization
// vendors and not to us. Root it is -- see docs/carplay_bringup.md.
//
// Three consequences worth having written down, because all three are load
// bearing and none is obvious from the libusb API:
//
//  * Capture does NOT re-enumerate the device. libusb's darwin backend follows
//    it with darwin_restore_state(), which closes and reopens the underlying
//    IOKit objects behind the same libusb_device_handle and puts the previous
//    configuration back. So the handle stays valid across a capture, and the
//    "do not re-enumerate" property switchToCarPlayConfiguration depends on
//    still holds here.
//
//  * Capture is refcounted on libusb's *cached device*, not on the handle, and
//    closing a handle does not release it. One capture therefore covers the
//    whole session, across every open/close the pipeline does -- the mux, the
//    lockdown channel and the NCM bridge each open their own handle and none of
//    them needs to capture again.
//
//  * Nothing in this library may ever call libusb_attach_kernel_driver() or
//    enable libusb_set_auto_detach_kernel_driver(). Either would drop the
//    capture refcount mid-session and hand the phone straight back to macOS's
//    own usbmuxd, which is running and will take it.
constexpr bool kCaptureIsWholeDevice =
#ifdef __APPLE__
    true;
#else
    false;
#endif

}  // namespace

// Declared in the header; see canDetachDevices() there for what it promises.
bool canDetachDevices(std::string& why_not)
{
#ifdef __APPLE__
    if (::geteuid() != 0)
    {
        why_not = "macOS only lets root take a USB device away from its own drivers "
                  "(the alternative, the com.apple.vm.device-access entitlement, is not "
                  "available to us). Re-run the node with sudo.";
        return false;
    }
#endif
    (void)why_not;
    return true;
}

namespace
{

// One context for the process. Deliberately never libusb_exit()'d: reader
// threads in MuxHost and NcmBridge outlive main()'s static destructors in a
// crash-stop shutdown, and tearing the context down underneath them is a
// use-after-free for the sake of an allocation the OS is about to reclaim.
libusb_context* context()
{
    static libusb_context* ctx = [] {
        libusb_context* c = nullptr;
        const int rc = libusb_init(&c);
        if (rc != LIBUSB_SUCCESS)
        {
            SPDLOG_ERROR("[usb] libusb_init failed: {}", libusb_strerror(rc));
            return static_cast<libusb_context*>(nullptr);
        }
        return c;
    }();
    return ctx;
}

// Map libusb's error codes onto the errno values the old usbfs wrappers threw,
// so existing callers keep working -- muxd's reader loop in particular tells a
// poll that found nothing (ETIMEDOUT) from a phone that went away (ENODEV).
int toErrno(int rc)
{
    switch (rc)
    {
        case LIBUSB_ERROR_TIMEOUT:
            return ETIMEDOUT;
        case LIBUSB_ERROR_NO_DEVICE:
            return ENODEV;
        case LIBUSB_ERROR_NOT_FOUND:
            return ENOENT;
        case LIBUSB_ERROR_ACCESS:
            return EACCES;
        case LIBUSB_ERROR_BUSY:
            return EBUSY;
        case LIBUSB_ERROR_PIPE:
            return EPIPE;
        case LIBUSB_ERROR_OVERFLOW:
            return EOVERFLOW;
        case LIBUSB_ERROR_INTERRUPTED:
            return EINTR;
        case LIBUSB_ERROR_NO_MEM:
            return ENOMEM;
        case LIBUSB_ERROR_NOT_SUPPORTED:
            return ENOSYS;
        case LIBUSB_ERROR_INVALID_PARAM:
            return EINVAL;
        default:
            return EIO;
    }
}

[[noreturn]] void throwUsb(const char* what, int rc)
{
    throw std::system_error(toErrno(rc), std::generic_category(),
                            std::string(what) + ": " + libusb_strerror(rc));
}

std::optional<PortPath> portPathOf(libusb_device* dev)
{
    PortPath path;
    path.bus = libusb_get_bus_number(dev);
    uint8_t ports[kMaxPortPathDepth] = {};
    const int n = libusb_get_port_numbers(dev, ports, static_cast<int>(sizeof(ports)));
    if (n <= 0)
    {
        // Root hubs have no port path. Nothing we care about is one.
        return std::nullopt;
    }
    path.ports.assign(ports, ports + n);
    return path;
}

// Walks the device list looking for one at `port`. The returned device carries
// a reference the caller must libusb_unref_device().
//
// Everything resolves a device this way rather than holding a libusb_device*
// across calls, because the CarPlay switch re-enumerates the phone and would
// leave any cached pointer stale. Re-listing costs a syscall or two and makes
// the stale case structurally impossible.
libusb_device* acquireDeviceAt(const PortPath& port)
{
    libusb_context* ctx = context();
    if (ctx == nullptr)
    {
        return nullptr;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0)
    {
        SPDLOG_ERROR("[usb] libusb_get_device_list failed: {}",
                     libusb_strerror(static_cast<int>(count)));
        return nullptr;
    }

    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count; ++i)
    {
        if (const auto p = portPathOf(list[i]); p && *p == port)
        {
            found = libusb_ref_device(list[i]);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

std::optional<DeviceInfo> describe(libusb_device* dev)
{
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS)
    {
        return std::nullopt;
    }

    const auto port = portPathOf(dev);
    if (!port)
    {
        return std::nullopt;
    }

    DeviceInfo info;
    info.port = *port;
    info.vid = desc.idVendor;
    info.pid = desc.idProduct;
    info.address = libusb_get_device_address(dev);
    info.num_configurations = desc.bNumConfigurations;

    // Available without opening the device: on Linux libusb reads it from
    // sysfs, on macOS from IOKit. An unconfigured device has none, which is
    // not an error -- it just leaves active_configuration at 0.
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) == LIBUSB_SUCCESS && cfg != nullptr)
    {
        info.active_configuration = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);
    }
    return info;
}

ConfigInfo toConfigInfo(const libusb_config_descriptor& cfg)
{
    ConfigInfo out;
    out.value = cfg.bConfigurationValue;
    for (uint8_t i = 0; i < cfg.bNumInterfaces; ++i)
    {
        const libusb_interface& iface = cfg.interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a)
        {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            InterfaceInfo info;
            info.number = alt.bInterfaceNumber;
            info.alt_setting = alt.bAlternateSetting;
            info.iface_class = alt.bInterfaceClass;
            info.subclass = alt.bInterfaceSubClass;
            info.protocol = alt.bInterfaceProtocol;
            if (alt.extra != nullptr && alt.extra_length > 0)
            {
                info.extra.assign(alt.extra, alt.extra + alt.extra_length);
            }
            for (uint8_t e = 0; e < alt.bNumEndpoints; ++e)
            {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                info.endpoints.push_back(EndpointInfo{ep.bEndpointAddress, ep.bmAttributes,
                                                      ep.wMaxPacketSize});
            }
            out.interfaces.push_back(std::move(info));
        }
    }
    return out;
}

}  // namespace

// ---------------- PortPath ----------------

std::string PortPath::toString() const
{
    std::string out = std::to_string(static_cast<unsigned>(bus));
    for (size_t i = 0; i < ports.size(); ++i)
    {
        out += (i == 0) ? '-' : '.';
        out += std::to_string(static_cast<unsigned>(ports[i]));
    }
    return out;
}

// ---------------- DeviceHandle ----------------

DeviceHandle::~DeviceHandle()
{
    reset();
}

DeviceHandle::DeviceHandle(DeviceHandle&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

DeviceHandle& DeviceHandle::operator=(DeviceHandle&& other) noexcept
{
    if (this != &other)
    {
        reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void DeviceHandle::reset()
{
    if (handle_ != nullptr)
    {
        libusb_close(handle_);
        handle_ = nullptr;
    }
}

// ---------------- enumeration ----------------

std::vector<DeviceInfo> listAppleDevices()
{
    std::vector<DeviceInfo> devices;

    libusb_context* ctx = context();
    if (ctx == nullptr)
    {
        return devices;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0)
    {
        SPDLOG_ERROR("[usb] libusb_get_device_list failed: {}",
                     libusb_strerror(static_cast<int>(count)));
        return devices;
    }

    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
        {
            continue;
        }
        if (desc.idVendor != kAppleVendorId)
        {
            continue;
        }
        if (auto info = describe(list[i]); info)
        {
            devices.push_back(std::move(*info));
        }
    }
    libusb_free_device_list(list, 1);
    return devices;
}

std::optional<DeviceInfo> findDeviceAt(const PortPath& port)
{
    libusb_device* dev = acquireDeviceAt(port);
    if (dev == nullptr)
    {
        return std::nullopt;
    }
    auto info = describe(dev);
    libusb_unref_device(dev);
    return info;
}

DeviceHandle openDevice(const PortPath& port)
{
    libusb_device* dev = acquireDeviceAt(port);
    if (dev == nullptr)
    {
        SPDLOG_ERROR("[usb] no device at port {}", port.toString());
        return DeviceHandle{};
    }

    libusb_device_handle* handle = nullptr;
    const int rc = libusb_open(dev, &handle);
    libusb_unref_device(dev);
    if (rc != LIBUSB_SUCCESS)
    {
        SPDLOG_ERROR("[usb] libusb_open({}) failed: {}", port.toString(), libusb_strerror(rc));
        return DeviceHandle{};
    }
    return DeviceHandle{handle};
}

DeviceHandle openDevice(const DeviceInfo& device)
{
    return openDevice(device.port);
}

// String descriptors come back padded. This phone reports its 24-character UDID
// in a 40-character field, space filled, and libusb hands back all 40 -- so the
// UDID arrives with sixteen trailing spaces on it.
//
// That was invisible for a long time. It does not show in a terminal, and it is
// harmless while both ends of the usbmux conversation are ours: UsbmuxdServer
// echoes back the same padded string it was given, so the comparison matches.
// It only breaks against someone else's mux, which is exactly what macOS is --
// and there it fails as "the mux does not list udid=...", which looks like a
// mux problem rather than a string problem.
namespace
{
std::string trimDescriptor(std::string text)
{
    const auto is_padding = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };
    while (!text.empty() && is_padding(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && is_padding(static_cast<unsigned char>(text[start])))
    {
        ++start;
    }
    return text.substr(start);
}

}  // namespace

std::string readSerial(const DeviceHandle& handle)
{
    if (!handle)
    {
        return {};
    }
    libusb_device* dev = libusb_get_device(handle.native());
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS || desc.iSerialNumber == 0)
    {
        return {};
    }

    unsigned char buffer[128] = {};
    const int n = libusb_get_string_descriptor_ascii(handle.native(), desc.iSerialNumber, buffer,
                                                     static_cast<int>(sizeof(buffer)));
    if (n <= 0)
    {
        SPDLOG_WARN("[usb] cannot read iSerialNumber: {}", libusb_strerror(n));
        return {};
    }
    return trimDescriptor(std::string(reinterpret_cast<const char*>(buffer),
                                      static_cast<size_t>(n)));
}

std::string readStringDescriptor(const DeviceHandle& handle, uint8_t index)
{
    if (!handle || index == 0)
    {
        return {};
    }

    unsigned char buffer[128] = {};
    const int n = libusb_get_string_descriptor_ascii(handle.native(), index, buffer,
                                                     static_cast<int>(sizeof(buffer)));
    if (n <= 0)
    {
        SPDLOG_DEBUG("[usb] cannot read string descriptor {}: {}", index, libusb_strerror(n));
        return {};
    }
    return trimDescriptor(std::string(reinterpret_cast<const char*>(buffer),
                                      static_cast<size_t>(n)));
}

std::optional<ConfigInfo> readActiveConfig(const PortPath& port)
{
    libusb_device* dev = acquireDeviceAt(port);
    if (dev == nullptr)
    {
        return std::nullopt;
    }

    libusb_config_descriptor* cfg = nullptr;
    const int rc = libusb_get_active_config_descriptor(dev, &cfg);
    libusb_unref_device(dev);
    if (rc != LIBUSB_SUCCESS || cfg == nullptr)
    {
        SPDLOG_DEBUG("[usb] no active config descriptor at port {}: {}", port.toString(),
                     libusb_strerror(rc));
        return std::nullopt;
    }

    ConfigInfo out = toConfigInfo(*cfg);
    libusb_free_config_descriptor(cfg);
    return out;
}

std::optional<ConfigInfo> readActiveConfig(const DeviceInfo& device)
{
    return readActiveConfig(device.port);
}

// ---------------- configuration ----------------

bool switchToCarPlayConfiguration(const DeviceInfo& device)
{
    const PortPath port = device.port;

    // Already there: nothing to do, and -- this is the point -- nothing to ask
    // permission for. The configuration is sticky across unplugs, so on macOS a
    // single privileged run switches the phone and every run after it can do
    // stages 3 onwards unprivileged. Checking before the preflight rather than
    // after is what makes that true.
    if (device.active_configuration == kCarPlayConfiguration)
    {
        SPDLOG_DEBUG("[usb] port {} is already in configuration {}", port.toString(),
                     kCarPlayConfiguration);
        return true;
    }

    // Fail here rather than several layers down. Without this the macOS story
    // is a LIBUSB_ERROR_ACCESS inside the detach followed by a "could not set
    // configuration 6" that says nothing about why.
    if (std::string why_not; !canDetachDevices(why_not))
    {
        SPDLOG_ERROR("[usb] cannot take the phone at port {} away from its drivers: {}",
                     port.toString(), why_not);
        return false;
    }

    // The Apple vendor request unlocks the CarPlay configurations; without it
    // the device only advertises configs 1-4. It is a one-shot control
    // transfer, after which the device re-enumerates.
    if (device.num_configurations < kCarPlayConfiguration)
    {
        {
            DeviceHandle handle = openDevice(port);
            if (!handle)
            {
                return false;
            }

            // On macOS take the device first. libusb tolerates an open that was
            // refused for exclusive access and still routes endpoint-0 requests
            // through DeviceRequestAsyncTO, but whether IOKit honours them while
            // macOS's own usbmuxd holds the phone is not something the API
            // promises either way. Capturing first makes the question moot, and
            // costs nothing if the request would have gone through regardless.
            // The vendor request re-enumerates the device for real, which may
            // drop this capture -- that is fine, the config switch below takes
            // it again.
            if (kCaptureIsWholeDevice)
            {
                if (const auto cfg = readActiveConfig(port); cfg)
                {
                    detachKernelDrivers(handle, *cfg);
                }
            }

            try
            {
                // wIndex 0x0004 mirrors LIVI's request; wLength 1 (device-to-host).
                usbControl(handle, 0xC0, 0x52, 0x0000, 0x0004, 1);
            }
            catch (const std::system_error& e)
            {
                SPDLOG_ERROR("[usb] Apple vendor request 0x52 failed: {}", e.what());
                return false;
            }
        }

        // Wait for it to come back at the same physical port with the CarPlay
        // configurations exposed. The address will have changed; the port has
        // not, which is exactly why the port path is what we track.
        bool exposed = false;
        for (int i = 0; i < 25; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (const auto d = findDeviceAt(port);
                d && d->num_configurations >= kCarPlayConfiguration)
            {
                exposed = true;
                break;
            }
        }
        if (!exposed)
        {
            SPDLOG_ERROR("[usb] device at port {} did not expose CarPlay configurations",
                         port.toString());
            return false;
        }
    }

    const auto current = findDeviceAt(port);
    if (!current)
    {
        SPDLOG_ERROR("[usb] device vanished from port {}", port.toString());
        return false;
    }
    if (current->active_configuration == kCarPlayConfiguration)
    {
        return true;
    }

    DeviceHandle handle = openDevice(port);
    if (handle)
    {
        // Linux returns EBUSY while any interface is claimed, so every bound
        // driver (ipheth, cdc_ncm, an earlier client) has to be released first.
        // macOS instead captures the whole device in one go -- and it is macOS's
        // own usbmuxd that has to be displaced, which is running by default and
        // will reclaim the phone the moment we let go. Either way the handle
        // survives; see kCaptureIsWholeDevice. Nothing else in this library
        // detaches drivers, which is why this is done explicitly here.
        if (const auto cfg = readActiveConfig(port); cfg)
        {
            detachKernelDrivers(handle, *cfg);
        }

        // libusb_set_configuration issues USBDEVFS_SETCONFIGURATION on Linux,
        // which -- unlike writing sysfs bConfigurationValue -- does not
        // re-enumerate the device, so open fds and, on a VM, the hypervisor's
        // passthrough binding both survive. macOS's capture does not
        // re-enumerate either, so the same reasoning holds there.
        const int rc = libusb_set_configuration(handle.native(), kCarPlayConfiguration);
        if (rc == LIBUSB_SUCCESS)
        {
            return true;
        }
        SPDLOG_DEBUG("[usb] libusb_set_configuration({}) failed: {}", kCarPlayConfiguration,
                     libusb_strerror(rc));
        handle.reset();
    }

    // No fallback on purpose. A root-only write to sysfs bConfigurationValue
    // used to sit here, inherited from the usbfs implementation; it was removed
    // once libusb was verified against hardware. It bought almost nothing --
    // root can open the usbfs node too, so "sysfs writable but libusb cannot
    // open the device" is a nearly empty set -- and it re-enumerated the device
    // to get there, which is the one thing this function is careful *not* to do.
    if (kCaptureIsWholeDevice)
    {
        // We are already root by the time we get here -- canCaptureDevices()
        // checked -- so the remaining suspect is contention, not permission.
        SPDLOG_ERROR("[usb] Failed to set configuration {} at port {}. The device was not "
                     "captured from macOS's own drivers; usbmuxd may have reclaimed it. "
                     "Unplug and replug the phone and try again.",
                     kCarPlayConfiguration, port.toString());
    }
    else
    {
        SPDLOG_ERROR("[usb] Failed to set configuration {} at port {}. Either run as root or "
                     "install nodes/carplay/udev/99-carplay.rules to get usbfs access.",
                     kCarPlayConfiguration, port.toString());
    }
    return false;
}

// ---------------- driver arbitration ----------------

bool kernelDriverActive(const DeviceHandle& handle, uint8_t iface)
{
    if (!handle)
    {
        return false;
    }
    return libusb_kernel_driver_active(handle.native(), iface) == 1;
}

bool detachKernelDriver(const DeviceHandle& handle, uint8_t iface)
{
    if (!handle)
    {
        return false;
    }
    // On macOS `iface` is ignored: the darwin backend captures the whole device
    // whichever interface it is handed. See kCaptureIsWholeDevice.
    const int rc = libusb_detach_kernel_driver(handle.native(), iface);
    if (rc == LIBUSB_SUCCESS)
    {
        return true;
    }
    // NOT_FOUND just means nothing was bound, which is the desired end state.
    if (rc == LIBUSB_ERROR_NOT_FOUND)
    {
        return false;
    }

    // The two macOS-specific failures are worth naming in full: both are
    // actionable, and both otherwise surface much later as an unexplained
    // "could not set configuration 6".
    if (kCaptureIsWholeDevice && rc == LIBUSB_ERROR_ACCESS)
    {
        SPDLOG_ERROR("[usb] refused permission to capture the device from the drivers that "
                     "hold it. Run the node as root on macOS.");
    }
    else if (kCaptureIsWholeDevice && rc == LIBUSB_ERROR_NOT_SUPPORTED)
    {
        SPDLOG_ERROR("[usb] this macOS is too old to capture USB devices (libusb needs "
                     "IOUSBHostDevice interface version 700 or newer).");
    }
    else
    {
        SPDLOG_DEBUG("[usb] detach kernel driver from interface {} failed: {}", iface,
                     libusb_strerror(rc));
    }
    return false;
}

unsigned detachKernelDrivers(const DeviceHandle& handle, const ConfigInfo& config)
{
    unsigned detached = 0;
    uint8_t last = 0xFF;
    for (const auto& iface : config.interfaces)
    {
        // interfaces holds every alt setting; only act once per interface.
        if (iface.number == last)
        {
            continue;
        }
        last = iface.number;
        if (!kernelDriverActive(handle, iface.number))
        {
            continue;
        }
        if (detachKernelDriver(handle, iface.number))
        {
            ++detached;
            if (kCaptureIsWholeDevice)
            {
                // That one call took every interface at once. Going round again
                // would only bump libusb's capture refcount, which nothing ever
                // decrements, and would log a per-interface line for something
                // that did not happen per interface.
                SPDLOG_DEBUG("[usb] captured the whole device from its drivers");
                break;
            }
            SPDLOG_DEBUG("[usb] released interface {} from its kernel driver", iface.number);
        }
        else if (kCaptureIsWholeDevice)
        {
            // Capture is all-or-nothing, so a failure here will not turn into a
            // success on the next interface. detachKernelDriver has already
            // explained why.
            break;
        }
    }
    return detached;
}

// ---------------- transfers ----------------

void usbClaimInterface(const DeviceHandle& handle, uint8_t iface)
{
    const int rc = libusb_claim_interface(handle.native(), iface);
    if (rc != LIBUSB_SUCCESS)
    {
        throwUsb("libusb_claim_interface", rc);
    }
}

void usbReleaseInterface(const DeviceHandle& handle, uint8_t iface)
{
    const int rc = libusb_release_interface(handle.native(), iface);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_NO_DEVICE)
    {
        SPDLOG_DEBUG("[usb] libusb_release_interface({}) failed: {}", iface, libusb_strerror(rc));
    }
}

void usbSetAltSetting(const DeviceHandle& handle, uint8_t iface, uint8_t alt_setting)
{
    const int rc = libusb_set_interface_alt_setting(handle.native(), iface, alt_setting);
    if (rc != LIBUSB_SUCCESS)
    {
        throwUsb("libusb_set_interface_alt_setting", rc);
    }
}

std::vector<uint8_t> usbControl(const DeviceHandle& handle, uint8_t bmRequestType,
                                uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                uint16_t wLength, const uint8_t* out_data, unsigned timeout_ms)
{
    std::vector<uint8_t> buffer(std::max<uint16_t>(wLength, 1));
    if (out_data != nullptr && wLength > 0)
    {
        std::copy_n(out_data, wLength, buffer.begin());
    }

    const int n = libusb_control_transfer(handle.native(), bmRequestType, bRequest, wValue, wIndex,
                                          buffer.data(), wLength, timeout_ms);
    if (n < 0)
    {
        throwUsb("libusb_control_transfer", n);
    }
    buffer.resize(std::min<size_t>(static_cast<size_t>(n), wLength));
    return buffer;
}

void usbBulkOut(const DeviceHandle& handle, uint8_t endpoint, const uint8_t* data, size_t len,
                unsigned timeout_ms)
{
    // libusb keeps the buffer non-const; copy into a scratch buffer.
    std::vector<uint8_t> scratch(data, data + len);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle.native(), endpoint, scratch.data(),
                                        static_cast<int>(scratch.size()), &transferred, timeout_ms);
    if (rc != LIBUSB_SUCCESS)
    {
        throwUsb("libusb_bulk_transfer(out)", rc);
    }
    if (static_cast<size_t>(transferred) != len)
    {
        throw std::system_error(EIO, std::generic_category(),
                                "libusb_bulk_transfer(out): short write");
    }
}

std::vector<uint8_t> usbBulkIn(const DeviceHandle& handle, uint8_t endpoint, size_t max_len,
                               unsigned timeout_ms)
{
    std::vector<uint8_t> buffer(max_len);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle.native(), endpoint, buffer.data(),
                                        static_cast<int>(buffer.size()), &transferred, timeout_ms);
    // A timeout that still moved data is not a failure: libusb reports both,
    // and the bytes are real. Only an empty timeout is the "nothing to read"
    // case that muxd's reader loop spins on.
    if (rc != LIBUSB_SUCCESS && !(rc == LIBUSB_ERROR_TIMEOUT && transferred > 0))
    {
        throwUsb("libusb_bulk_transfer(in)", rc);
    }
    buffer.resize(static_cast<size_t>(transferred));
    return buffer;
}

bool usbClearHalt(const DeviceHandle& handle, uint8_t endpoint)
{
    const int rc = libusb_clear_halt(handle.native(), endpoint);
    if (rc != LIBUSB_SUCCESS)
    {
        SPDLOG_DEBUG("[usb] libusb_clear_halt(0x{:02x}) failed: {}", endpoint,
                     libusb_strerror(rc));
        return false;
    }
    return true;
}

}  // namespace apple_usb
