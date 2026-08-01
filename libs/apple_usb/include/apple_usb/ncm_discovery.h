// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_NCM_DISCOVERY_H_
#define APPLE_USB_NCM_DISCOVERY_H_

#include "apple_usb/usb_device.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apple_usb
{

// CDC class codes, as they appear in interface descriptors.
inline constexpr uint8_t kCdcInterfaceClass = 0x02;
inline constexpr uint8_t kCdcNcmSubClass = 0x0d;
inline constexpr uint8_t kCdcDataInterfaceClass = 0x0a;

// The NCM data interface carries its bulk endpoints on altsetting 1;
// altsetting 0 is the mandatory "no data" setting.
inline constexpr uint8_t kNcmDataAltSetting = 1;

// One CDC-NCM function: a control interface and the data interface that
// follows it, with every endpoint already resolved.
//
// An iPhone in the CarPlay configuration exposes *two* of these, and which one
// carries the AV link is not self-evident from the descriptors -- hence
// findNcmFunctions() returning all of them in descriptor order rather than
// picking one.
struct NcmFunction
{
    uint8_t ctrl_iface = 0;
    uint8_t data_iface = 0;

    // Bulk pair on the data interface's altsetting 1.
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;

    // Status endpoint on the control interface. Zero when absent, which is
    // legal -- the bridge logs it and carries on.
    uint8_t ep_int = 0;

    // iMACAddress from the CDC Ethernet Networking functional descriptor: the
    // string-descriptor index holding the MAC the phone insists the host use.
    // Zero when the descriptor is missing.
    uint8_t mac_string_index = 0;

    bool hasBulkPair() const { return ep_in != 0 && ep_out != 0; }
};

// Every CDC-NCM function in a configuration, in interface-number order.
//
// A function qualifies when a communications(0x02)/NCM(0x0d) control interface
// is immediately followed by a CDC-data(0x0a) interface. Endpoints come
// straight from the descriptor, so this can run *before* the data altsetting
// is selected -- the sysfs version it replaces could only see them afterwards.
std::vector<NcmFunction> findNcmFunctions(const ConfigInfo& config);

// The function whose control interface is `ctrl_iface`, if present. Used by
// the CARPLAY_NCM_CTRL_IF bring-up override.
std::optional<NcmFunction> findNcmFunctionByCtrl(const ConfigInfo& config, uint8_t ctrl_iface);

// Pull iMACAddress out of an interface's class-specific descriptors. Returns 0
// when there is no Ethernet Networking functional descriptor.
//
// libusb has already scoped `extra` to one interface, so unlike the walk over
// /sys/.../descriptors that this replaces, there is no need to track which
// interface each descriptor belonged to.
uint8_t macStringIndexFromExtra(const std::vector<uint8_t>& extra);

// Decode a USB string descriptor holding a 12-character hex MAC (the format
// iMACAddress uses) into "aa:bb:cc:dd:ee:ff". Returns empty on anything
// malformed. `descriptor` is the raw transfer, starting at bLength.
std::string macFromStringDescriptor(const std::vector<uint8_t>& descriptor);

}  // namespace apple_usb

#endif  // APPLE_USB_NCM_DISCOVERY_H_
