// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
#include "apple_usb/usb_device.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#ifdef __linux__
#include <linux/usbdevice_fs.h>
#endif

namespace fs = std::filesystem;

namespace apple_usb
{

namespace
{

std::string readSysfsAttr(const fs::path& device_dir, const char* attr)
{
    std::ifstream in(device_dir / attr);
    std::string value;
    std::getline(in, value);
    return value;
}

uint32_t readSysfsUint(const fs::path& device_dir, const char* attr, int base)
{
    const std::string text = readSysfsAttr(device_dir, attr);
    uint32_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value, base);
    return value;
}

}  // namespace

std::vector<DeviceInfo> listAppleDevices()
{
    std::vector<DeviceInfo> devices;

    const fs::path root{"/sys/bus/usb/devices"};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec))
    {
        const fs::path& dir = entry.path();
        // Skip interfaces (1-1:1.0) and root hubs (usb1).
        const std::string name = dir.filename().string();
        if (name.find(':') != std::string::npos || name.starts_with("usb"))
        {
            continue;
        }

        const auto vid = static_cast<uint16_t>(readSysfsUint(dir, "idVendor", 16));
        if (vid != kAppleVendorId)
        {
            continue;
        }

        DeviceInfo info;
        info.sysfs_path = dir.string();
        info.vid = vid;
        info.pid = static_cast<uint16_t>(readSysfsUint(dir, "idProduct", 16));
        info.serial = readSysfsAttr(dir, "serial");
        info.active_configuration = static_cast<uint8_t>(readSysfsUint(dir, "bConfigurationValue", 10));

        const uint32_t busnum = readSysfsUint(dir, "busnum", 10);
        const uint32_t devnum = readSysfsUint(dir, "devnum", 10);
        info.usbfs_path = fmt::format("/dev/bus/usb/{:03d}/{:03d}", busnum, devnum);

        devices.push_back(std::move(info));
    }

    return devices;
}

std::optional<int> openDevice(const DeviceInfo& device)
{
    const int fd = ::open(device.usbfs_path.c_str(), O_RDWR);
    if (fd < 0)
    {
        SPDLOG_ERROR("Failed to open {}: {}", device.usbfs_path, strerror(errno));
        return std::nullopt;
    }
    return fd;
}

#ifdef __linux__

namespace
{

[[noreturn]] void throwErrno(const char* what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

std::optional<DeviceInfo> findBySerial(const std::string& serial)
{
    for (auto& dev : listAppleDevices())
    {
        if (dev.serial == serial)
        {
            return dev;
        }
    }
    return std::nullopt;
}

uint8_t configFromSysfs(const std::string& sysfs_path)
{
    return static_cast<uint8_t>(readSysfsUint(fs::path(sysfs_path), "bConfigurationValue", 10));
}

}  // namespace

bool switchToCarPlayConfiguration(const DeviceInfo& device)
{
    // The Apple vendor request unlocks the CarPlay configurations; without it
    // the device only advertises configs 1-4. It is a one-shot control
    // transfer that we issue over a short-lived usbfs fd.
    if (static_cast<uint8_t>(readSysfsUint(fs::path(device.sysfs_path), "bNumConfigurations", 10)) < 6)
    {
        const auto fd = openDevice(device);
        if (!fd)
        {
            return false;
        }
        try
        {
            // wIndex 0x0004 mirrors LIVI's request; wLength 1 (device-to-host).
            usbControl(*fd, 0xC0, 0x52, 0x0000, 0x0004, 1);
        }
        catch (const std::system_error& e)
        {
            SPDLOG_ERROR("Apple vendor request 0x52 failed: {}", e.what());
            ::close(*fd);
            return false;
        }
        ::close(*fd);

        // The device re-enumerates; wait for the CarPlay configs to appear.
        bool exposed = false;
        for (int i = 0; i < 25; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (const auto d = findBySerial(device.serial);
                d && readSysfsUint(fs::path(d->sysfs_path), "bNumConfigurations", 10) >= 6)
            {
                exposed = true;
                break;
            }
        }
        if (!exposed)
        {
            SPDLOG_ERROR("iPhone {} did not expose CarPlay configurations", device.serial);
            return false;
        }
    }

    const auto d = findBySerial(device.serial);
    if (!d)
    {
        return false;
    }
    if (configFromSysfs(d->sysfs_path) == kCarPlayConfiguration)
    {
        return true;
    }

    // Prefer usbfs SET_CONFIGURATION: it needs only the usbfs node (which a
    // udev rule can hand to a normal user) whereas the sysfs attribute is
    // root-only, and unlike the vendor request above it does not re-enumerate
    // the device -- so open fds and, on a VM, the hypervisor's passthrough
    // binding both survive.
    //
    // The kernel returns EBUSY while any interface is claimed, so every bound
    // driver (ipheth, cdc_ncm, an earlier usbfs client) has to be released
    // first. Nothing else in this library detaches drivers, which is why this
    // is done explicitly here.
    if (const auto fd = openDevice(*d); fd)
    {
        for (const auto iface : boundInterfaces(*d))
        {
            if (usbDisconnectKernelDriver(*fd, iface))
            {
                SPDLOG_DEBUG("released interface {} from its kernel driver", iface);
            }
        }

        unsigned int configuration = kCarPlayConfiguration;
        const bool ok = ::ioctl(*fd, USBDEVFS_SETCONFIGURATION, &configuration) >= 0;
        const int saved_errno = errno;
        ::close(*fd);

        if (ok)
        {
            return true;
        }
        SPDLOG_DEBUG("USBDEVFS_SETCONFIGURATION({}) failed: {}; falling back to sysfs",
                     kCarPlayConfiguration, std::strerror(saved_errno));
    }

    // Fallback for hosts where the usbfs node is unavailable but we are root.
    std::ofstream out(fs::path(d->sysfs_path) / "bConfigurationValue");
    out << static_cast<int>(kCarPlayConfiguration);
    out.close();
    if (!out)
    {
        SPDLOG_ERROR("Failed to set configuration {} for {}: the usbfs ioctl failed and "
                     "{}/bConfigurationValue is not writable (it is root-only). Either run "
                     "as root or install nodes/carplay/udev/99-carplay.rules.",
                     kCarPlayConfiguration, d->serial, d->sysfs_path);
        return false;
    }
    return true;
}

std::vector<unsigned int> boundInterfaces(const DeviceInfo& device)
{
    std::vector<unsigned int> interfaces;
    const fs::path sysfs(device.sysfs_path);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sysfs.parent_path(), ec))
    {
        // Interface directories are named "<device>:<config>.<interface>".
        const std::string name = entry.path().filename().string();
        if (name.rfind(sysfs.filename().string() + ":", 0) != 0)
        {
            continue;
        }
        const auto dot = name.rfind('.');
        if (dot == std::string::npos)
        {
            continue;
        }

        if (!fs::exists(entry.path() / "driver", ec))
        {
            continue;
        }

        unsigned int iface = 0;
        const std::string number = name.substr(dot + 1);
        if (std::from_chars(number.data(), number.data() + number.size(), iface).ec ==
            std::errc{})
        {
            interfaces.push_back(iface);
        }
    }
    return interfaces;
}

bool usbDisconnectKernelDriver(int fd, unsigned int iface)
{
    usbdevfs_ioctl command{};
    command.ifno = static_cast<int>(iface);
    command.ioctl_code = USBDEVFS_DISCONNECT;
    command.data = nullptr;

    if (::ioctl(fd, USBDEVFS_IOCTL, &command) < 0)
    {
        SPDLOG_DEBUG("USBDEVFS_DISCONNECT on interface {} failed: {}", iface,
                     std::strerror(errno));
        return false;
    }
    return true;
}

void usbClaimInterface(int fd, unsigned int iface)
{
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0)
    {
        throwErrno("USBDEVFS_CLAIMINTERFACE");
    }
}

std::vector<uint8_t> usbControl(int fd, uint8_t bmRequestType, uint8_t bRequest,
                                uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                const uint8_t* out_data, unsigned timeout_ms)
{
    std::vector<uint8_t> buffer(std::max<uint16_t>(wLength, 1));
    if (out_data != nullptr && wLength > 0)
    {
        std::copy_n(out_data, wLength, buffer.begin());
    }

    usbdevfs_ctrltransfer req{};
    req.bRequestType = bmRequestType;
    req.bRequest = bRequest;
    req.wValue = wValue;
    req.wIndex = wIndex;
    req.wLength = wLength;
    req.timeout = timeout_ms;
    req.data = buffer.data();

    const int n = ioctl(fd, USBDEVFS_CONTROL, &req);
    if (n < 0)
    {
        throwErrno("USBDEVFS_CONTROL");
    }
    buffer.resize(std::min<size_t>(static_cast<size_t>(n), wLength));
    return buffer;
}

bool usbClearHalt(int fd, uint8_t endpoint)
{
    unsigned int target = endpoint;
    if (ioctl(fd, USBDEVFS_CLEAR_HALT, &target) < 0)
    {
        SPDLOG_DEBUG("USBDEVFS_CLEAR_HALT(0x{:02x}) failed: {}", endpoint, std::strerror(errno));
        return false;
    }
    return true;
}

void usbBulkOut(int fd, uint8_t endpoint, const uint8_t* data, size_t len, unsigned timeout_ms)
{
    // usbdevfs_bulktransfer keeps data non-const; copy into a scratch buffer.
    std::vector<uint8_t> scratch(data, data + len);
    usbdevfs_bulktransfer req{};
    req.ep = endpoint;
    req.len = static_cast<unsigned>(len);
    req.timeout = timeout_ms;
    req.data = scratch.data();
    if (ioctl(fd, USBDEVFS_BULK, &req) < 0)
    {
        throwErrno("USBDEVFS_BULK(out)");
    }
}

std::vector<uint8_t> usbBulkIn(int fd, uint8_t endpoint, size_t max_len, unsigned timeout_ms)
{
    std::vector<uint8_t> buffer(max_len);
    usbdevfs_bulktransfer req{};
    req.ep = endpoint;
    req.len = static_cast<unsigned>(max_len);
    req.timeout = timeout_ms;
    req.data = buffer.data();
    const int n = ioctl(fd, USBDEVFS_BULK, &req);
    if (n < 0)
    {
        throwErrno("USBDEVFS_BULK(in)");
    }
    buffer.resize(static_cast<size_t>(n));
    return buffer;
}

#else  // not __linux__

bool switchToCarPlayConfiguration(const DeviceInfo&)
{
    SPDLOG_ERROR("CarPlay USB configuration switch is only supported on Linux");
    return false;
}

std::vector<unsigned int> boundInterfaces(const DeviceInfo&) { return {}; }
bool usbDisconnectKernelDriver(int, unsigned int) { return false; }
bool usbClearHalt(int, uint8_t) { return false; }

void usbClaimInterface(int, unsigned int) { throw std::system_error(ENOSYS, std::generic_category()); }
std::vector<uint8_t> usbControl(int, uint8_t, uint8_t, uint16_t, uint16_t, uint16_t, const uint8_t*, unsigned)
{
    throw std::system_error(ENOSYS, std::generic_category());
}
void usbBulkOut(int, uint8_t, const uint8_t*, size_t, unsigned) { throw std::system_error(ENOSYS, std::generic_category()); }
std::vector<uint8_t> usbBulkIn(int, uint8_t, size_t, unsigned) { throw std::system_error(ENOSYS, std::generic_category()); }

#endif  // __linux__

}  // namespace apple_usb
