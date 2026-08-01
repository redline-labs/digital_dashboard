// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/ncm_bridge.py
//
// Everything here is a pure function of a configuration descriptor, which is
// deliberate: it is the half of the NCM bridge that used to be spread across
// four sysfs walks, and making it pure is what lets it be unit tested without
// a phone (see test_ncm_discovery.cpp) and compiled on every platform even
// though the bridge it feeds is Linux-only.
#include "apple_usb/ncm_discovery.h"

#include <algorithm>
#include <cctype>

namespace apple_usb
{

namespace
{

// CDC functional descriptors live in an interface's class-specific bytes.
constexpr uint8_t kDescriptorTypeCsInterface = 0x24;
constexpr uint8_t kCdcEthernetFunctionalDescriptor = 0x0f;

const InterfaceInfo* findAltSetting(const ConfigInfo& config, uint8_t iface, uint8_t alt)
{
    for (const auto& candidate : config.interfaces)
    {
        if (candidate.number == iface && candidate.alt_setting == alt)
        {
            return &candidate;
        }
    }
    return nullptr;
}

}  // namespace

uint8_t macStringIndexFromExtra(const std::vector<uint8_t>& extra)
{
    size_t idx = 0;
    while (idx + 2 <= extra.size())
    {
        const uint8_t blen = extra[idx];
        if (blen < 2 || idx + blen > extra.size())
        {
            break;  // malformed; nothing useful past here
        }
        const uint8_t btype = extra[idx + 1];
        if (btype == kDescriptorTypeCsInterface && blen >= 4 &&
            extra[idx + 2] == kCdcEthernetFunctionalDescriptor)
        {
            return extra[idx + 3];
        }
        idx += blen;
    }
    return 0;
}

std::string macFromStringDescriptor(const std::vector<uint8_t>& descriptor)
{
    // bLength, bDescriptorType(0x03), then UTF-16LE code units.
    if (descriptor.size() < 2 || descriptor[1] != 0x03)
    {
        return {};
    }

    // bLength is authoritative but may exceed what the transfer returned.
    const size_t end = std::min<size_t>(descriptor[0], descriptor.size());

    std::string hex;
    for (size_t i = 2; i + 1 < end; i += 2)
    {
        // The MAC is plain ASCII hex, so the high byte of each code unit is
        // zero; anything else is not part of it.
        if (descriptor[i + 1] != 0)
        {
            continue;
        }
        hex.push_back(static_cast<char>(descriptor[i]));
    }
    if (hex.size() != 12)
    {
        return {};
    }
    if (!std::all_of(hex.begin(), hex.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; }))
    {
        return {};
    }

    std::string mac;
    for (size_t i = 0; i < 12; i += 2)
    {
        if (!mac.empty())
        {
            mac.push_back(':');
        }
        mac.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i]))));
        mac.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i + 1]))));
    }
    return mac;
}

std::vector<NcmFunction> findNcmFunctions(const ConfigInfo& config)
{
    std::vector<NcmFunction> functions;

    for (const auto& ctrl : config.interfaces)
    {
        // Control interfaces are described once, on altsetting 0.
        if (ctrl.alt_setting != 0)
        {
            continue;
        }
        if (ctrl.iface_class != kCdcInterfaceClass || ctrl.subclass != kCdcNcmSubClass)
        {
            continue;
        }

        // The data interface is the next one; the descriptor layout guarantees
        // the pairing, and the union functional descriptor would only restate
        // it.
        const uint8_t data_number = static_cast<uint8_t>(ctrl.number + 1);
        const InterfaceInfo* data0 = findAltSetting(config, data_number, 0);
        if (data0 == nullptr || data0->iface_class != kCdcDataInterfaceClass)
        {
            continue;
        }

        NcmFunction fn;
        fn.ctrl_iface = ctrl.number;
        fn.data_iface = data_number;
        fn.mac_string_index = macStringIndexFromExtra(ctrl.extra);

        for (const auto& ep : ctrl.endpoints)
        {
            if (ep.type() == TransferType::Interrupt && ep.isIn())
            {
                fn.ep_int = ep.address;
                break;
            }
        }

        // Bulk endpoints only exist on the data altsetting.
        if (const InterfaceInfo* data = findAltSetting(config, data_number, kNcmDataAltSetting);
            data != nullptr)
        {
            for (const auto& ep : data->endpoints)
            {
                if (ep.type() != TransferType::Bulk)
                {
                    continue;
                }
                (ep.isIn() ? fn.ep_in : fn.ep_out) = ep.address;
            }
        }

        functions.push_back(fn);
    }

    std::sort(functions.begin(), functions.end(),
              [](const NcmFunction& a, const NcmFunction& b) {
                  return a.ctrl_iface < b.ctrl_iface;
              });
    return functions;
}

std::optional<NcmFunction> findNcmFunctionByCtrl(const ConfigInfo& config, uint8_t ctrl_iface)
{
    for (const auto& fn : findNcmFunctions(config))
    {
        if (fn.ctrl_iface == ctrl_iface)
        {
            return fn;
        }
    }
    return std::nullopt;
}

}  // namespace apple_usb
