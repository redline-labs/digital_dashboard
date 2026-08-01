// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only enumeration and descriptor dump for attached Apple devices.
//
// This exists because the libusb port replaced a pile of sysfs parsing with
// descriptor walking, and every assumption that rewrite makes -- which
// interface carries the mux, which alt setting has the bulk endpoints, where
// the CDC Ethernet functional descriptor lives -- is checkable against a real
// phone *before* anything claims an interface or changes a configuration.
//
// Nothing here writes to the device. The one exception is opt-in: --serial
// opens the device to read its iSerialNumber string descriptor, which is a
// control transfer on endpoint 0 and does not disturb any bound driver.
//
//     ./build/libs/apple_usb/apple_usb_usbprobe [--serial]
#include "apple_usb/usb_device.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace
{

const char* transferTypeName(apple_usb::TransferType type)
{
    switch (type)
    {
        case apple_usb::TransferType::Control:
            return "control";
        case apple_usb::TransferType::Isochronous:
            return "isoc";
        case apple_usb::TransferType::Bulk:
            return "bulk";
        case apple_usb::TransferType::Interrupt:
            return "interrupt";
    }
    return "?";
}

// Decode the class-specific descriptors libusb hands back on an interface.
// For a CDC control interface these are the functional descriptors, and
// subtype 0x0f is the Ethernet Networking one whose iMACAddress field the NCM
// bridge needs. Printing the raw bytes alongside makes a mismatch obvious.
void dumpExtra(const std::vector<uint8_t>& extra)
{
    size_t idx = 0;
    while (idx + 2 <= extra.size())
    {
        const uint8_t blen = extra[idx];
        const uint8_t btype = extra[idx + 1];
        if (blen < 2 || idx + blen > extra.size())
        {
            std::printf("        malformed descriptor at offset %zu (bLength=%u)\n", idx, blen);
            break;
        }

        std::printf("        desc type=0x%02x len=%u", btype, blen);
        // 0x24 = CS_INTERFACE; byte 2 is the CDC functional descriptor subtype.
        if (btype == 0x24 && blen >= 3)
        {
            const uint8_t subtype = extra[idx + 2];
            std::printf(" cdc_subtype=0x%02x", subtype);
            if (subtype == 0x0f && blen >= 4)
            {
                std::printf(" (Ethernet Networking, iMACAddress=%u)", extra[idx + 3]);
            }
        }
        std::printf(" [");
        for (uint8_t b = 0; b < blen; ++b)
        {
            std::printf("%s%02x", b ? " " : "", extra[idx + b]);
        }
        std::printf("]\n");
        idx += blen;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    bool want_serial = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--serial") == 0)
        {
            want_serial = true;
        }
        else
        {
            std::fprintf(stderr,
                         "usage: %s [--serial]\n\n"
                         "Dumps every attached Apple device: physical port path, "
                         "configuration\ndescriptor, interfaces and endpoints. Read-only "
                         "unless --serial is given,\nwhich additionally opens each device to "
                         "read its UDID.\n",
                         argv[0]);
            return 2;
        }
    }

    spdlog::set_level(spdlog::level::debug);

    const auto devices = apple_usb::listAppleDevices();
    std::printf("Apple devices: %zu\n", devices.size());
    if (devices.empty())
    {
        std::printf("\nNothing found. If a phone is plugged in, this is a permissions "
                    "problem:\n  Linux: install nodes/carplay/udev/99-carplay.rules, or run "
                    "as root.\n  macOS: reading descriptors should work unprivileged; claiming "
                    "will not.\n");
        return 1;
    }

    for (const auto& device : devices)
    {
        std::printf("\n%04x:%04x  port=%s  address=%u  config=%u  nconfigs=%u\n", device.vid,
                    device.pid, device.port.toString().c_str(), device.address,
                    device.active_configuration, device.num_configurations);

        if (device.active_configuration == apple_usb::kCarPlayConfiguration)
        {
            std::printf("  ** already in the CarPlay configuration (%u) **\n",
                        apple_usb::kCarPlayConfiguration);
        }
        else if (device.num_configurations < apple_usb::kCarPlayConfiguration)
        {
            std::printf("  CarPlay configurations not yet unlocked (needs vendor request 0x52)\n");
        }

        if (want_serial)
        {
            apple_usb::DeviceHandle handle = apple_usb::openDevice(device);
            if (!handle)
            {
                std::printf("  serial: <could not open device>\n");
            }
            else
            {
                const std::string serial = apple_usb::readSerial(handle);
                std::printf("  serial: %s\n", serial.empty() ? "<unreadable>" : serial.c_str());
            }
        }

        const auto config = apple_usb::readActiveConfig(device);
        if (!config)
        {
            std::printf("  <no active configuration descriptor>\n");
            continue;
        }

        std::printf("  configuration %u, %zu interface alt settings\n", config->value,
                    config->interfaces.size());
        for (const auto& iface : config->interfaces)
        {
            std::printf("    if %u alt %u  class=%02x/%02x/%02x  endpoints=%zu  extra=%zu",
                        iface.number, iface.alt_setting, iface.iface_class, iface.subclass,
                        iface.protocol, iface.endpoints.size(), iface.extra.size());

            // Annotate the interfaces this stack actually looks for.
            if (iface.iface_class == 0xFF && iface.subclass == 0xFE && iface.protocol == 0x02)
            {
                std::printf("   <- usbmux");
            }
            else if (iface.iface_class == 0x02 && iface.subclass == 0x0D)
            {
                std::printf("   <- CDC-NCM control");
            }
            else if (iface.iface_class == 0x0A)
            {
                std::printf("   <- CDC data");
            }
            std::printf("\n");

            for (const auto& ep : iface.endpoints)
            {
                std::printf("        ep 0x%02x  %-9s %s  max_packet=%u\n", ep.address,
                            transferTypeName(ep.type()), ep.isIn() ? "IN " : "OUT",
                            ep.max_packet_size);
            }
            if (!iface.extra.empty())
            {
                dumpExtra(iface.extra);
            }
        }
    }

    std::printf("\n");
    return 0;
}
