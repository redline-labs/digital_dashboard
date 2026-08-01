// SPDX-License-Identifier: GPL-3.0-or-later
//
// Descriptor-driven NCM discovery, tested against synthetic configuration
// descriptors.
//
// This suite exists because the libusb port replaced four sysfs walks with
// descriptor parsing, and there was no hardware available to verify the
// rewrite against. The shapes below encode what an iPhone in the CarPlay
// configuration is *expected* to look like -- two NCM function pairs, bulk
// endpoints only on altsetting 1, iMACAddress in a CDC functional descriptor.
//
// When a phone is finally attached, run apple_usb_usbprobe and compare its
// output against carPlayLikeConfig() below. If they disagree, this file is
// what needs correcting, and these tests will then pin the real shape.
#include "apple_usb/ncm_discovery.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

template <typename T>
void expectEq(const T& actual, const T& expected, const std::string& what)
{
    if (actual != expected)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {} (got {}, expected {})", what, actual, expected);
    }
}

using apple_usb::ConfigInfo;
using apple_usb::EndpointInfo;
using apple_usb::InterfaceInfo;

constexpr uint8_t kBulk = 0x02;
constexpr uint8_t kInterrupt = 0x03;

InterfaceInfo iface(uint8_t number, uint8_t alt, uint8_t cls, uint8_t sub,
                    std::vector<EndpointInfo> endpoints = {}, std::vector<uint8_t> extra = {})
{
    InterfaceInfo info;
    info.number = number;
    info.alt_setting = alt;
    info.iface_class = cls;
    info.subclass = sub;
    info.endpoints = std::move(endpoints);
    info.extra = std::move(extra);
    return info;
}

EndpointInfo endpoint(uint8_t address, uint8_t attributes)
{
    return EndpointInfo{address, attributes, 512};
}

// A CDC Ethernet Networking functional descriptor (subtype 0x0f) preceded by a
// header functional descriptor, so the walk has to actually skip something.
std::vector<uint8_t> ethernetFunctionalDescriptors(uint8_t mac_string_index)
{
    return {
        0x05, 0x24, 0x00, 0x10, 0x01,                          // header, subtype 0x00
        0x0D, 0x24, 0x0F, mac_string_index, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // ethernet, subtype 0x0f
    };
}

// Two NCM function pairs, the way an iPhone in configuration 6 is expected to
// present: a vendor-specific mux interface first, then ctrl/data at 2/3 and
// again at 4/5. Bulk endpoints appear only on data altsetting 1.
ConfigInfo carPlayLikeConfig()
{
    ConfigInfo config;
    config.value = 6;
    config.interfaces = {
        // usbmux (what MuxHost looks for), deliberately not an NCM function.
        iface(1, 0, 0xFF, 0xFE, {endpoint(0x85, kBulk), endpoint(0x04, kBulk)}),

        // First NCM function.
        iface(2, 0, 0x02, 0x0d, {endpoint(0x86, kInterrupt)},
              ethernetFunctionalDescriptors(7)),
        iface(3, 0, 0x0a, 0x00),  // no-data altsetting: no endpoints
        iface(3, 1, 0x0a, 0x00, {endpoint(0x87, kBulk), endpoint(0x05, kBulk)}),

        // Second NCM function.
        iface(4, 0, 0x02, 0x0d, {endpoint(0x88, kInterrupt)},
              ethernetFunctionalDescriptors(8)),
        iface(5, 0, 0x0a, 0x00),
        iface(5, 1, 0x0a, 0x00, {endpoint(0x89, kBulk), endpoint(0x06, kBulk)}),
    };
    return config;
}

void testFindsBothFunctionsInOrder()
{
    const auto functions = apple_usb::findNcmFunctions(carPlayLikeConfig());
    expectEq(functions.size(), size_t{2}, "two NCM functions found");
    if (functions.size() != 2)
    {
        return;
    }

    // Order matters: the bridge takes the first, matching the Python's
    // sorted(os.listdir(...)) behaviour that the sysfs version relied on.
    expectEq(functions[0].ctrl_iface, uint8_t{2}, "first function control interface");
    expectEq(functions[0].data_iface, uint8_t{3}, "first function data interface");
    expectEq(functions[1].ctrl_iface, uint8_t{4}, "second function control interface");
    expectEq(functions[1].data_iface, uint8_t{5}, "second function data interface");
}

void testResolvesEndpointsFromDataAltSetting()
{
    const auto functions = apple_usb::findNcmFunctions(carPlayLikeConfig());
    if (functions.empty())
    {
        expect(false, "no functions to check endpoints on");
        return;
    }

    // The whole point of the descriptor rewrite: these come from altsetting 1
    // without having selected it first. The sysfs version could not see them
    // until after USBDEVFS_SETINTERFACE, which forced an ordering dependency.
    expectEq(functions[0].ep_in, uint8_t{0x87}, "bulk IN from data altsetting 1");
    expectEq(functions[0].ep_out, uint8_t{0x05}, "bulk OUT from data altsetting 1");
    expectEq(functions[0].ep_int, uint8_t{0x86}, "interrupt endpoint from control interface");
    expect(functions[0].hasBulkPair(), "first function has a complete bulk pair");

    expectEq(functions[1].ep_in, uint8_t{0x89}, "second function bulk IN");
    expectEq(functions[1].ep_out, uint8_t{0x06}, "second function bulk OUT");
}

void testExtractsMacStringIndex()
{
    const auto functions = apple_usb::findNcmFunctions(carPlayLikeConfig());
    if (functions.size() != 2)
    {
        expect(false, "need both functions for the iMACAddress check");
        return;
    }
    expectEq(functions[0].mac_string_index, uint8_t{7}, "first function iMACAddress index");
    expectEq(functions[1].mac_string_index, uint8_t{8}, "second function iMACAddress index");
}

void testMacStringIndexEdgeCases()
{
    expectEq(apple_usb::macStringIndexFromExtra({}), uint8_t{0}, "empty extra yields 0");

    // Header functional descriptor only: no ethernet descriptor to find.
    expectEq(apple_usb::macStringIndexFromExtra({0x05, 0x24, 0x00, 0x10, 0x01}), uint8_t{0},
             "no ethernet functional descriptor yields 0");

    // A zero bLength would loop forever if the walk did not guard it.
    expectEq(apple_usb::macStringIndexFromExtra({0x00, 0x24, 0x0f, 0x07}), uint8_t{0},
             "zero-length descriptor does not hang or misparse");

    // bLength running past the buffer must not read out of bounds.
    expectEq(apple_usb::macStringIndexFromExtra({0x40, 0x24, 0x0f, 0x07}), uint8_t{0},
             "overlong bLength is rejected");
}

void testRejectsUnpairedControlInterface()
{
    ConfigInfo config;
    config.value = 6;
    // An NCM control interface whose successor is not CDC-data.
    config.interfaces = {
        iface(2, 0, 0x02, 0x0d, {endpoint(0x86, kInterrupt)}),
        iface(3, 0, 0xFF, 0x00),
    };
    expect(apple_usb::findNcmFunctions(config).empty(),
           "control interface without a CDC-data successor is not a function");
}

void testMissingDataAltSettingLeavesBulkUnset()
{
    ConfigInfo config;
    config.value = 6;
    // Data interface present but with no altsetting 1, so no bulk endpoints.
    config.interfaces = {
        iface(2, 0, 0x02, 0x0d, {endpoint(0x86, kInterrupt)}),
        iface(3, 0, 0x0a, 0x00),
    };
    const auto functions = apple_usb::findNcmFunctions(config);
    expectEq(functions.size(), size_t{1}, "function is still recognised");
    if (functions.size() == 1)
    {
        expect(!functions[0].hasBulkPair(),
               "missing data altsetting leaves the bulk pair unset rather than guessing");
    }
}

void testFindByCtrlInterface()
{
    const ConfigInfo config = carPlayLikeConfig();
    const auto pinned = apple_usb::findNcmFunctionByCtrl(config, 4);
    expect(pinned.has_value(), "CARPLAY_NCM_CTRL_IF=4 resolves");
    if (pinned)
    {
        expectEq(pinned->data_iface, uint8_t{5}, "pinned function data interface");
        expectEq(pinned->ep_in, uint8_t{0x89}, "pinned function bulk IN");
    }
    expect(!apple_usb::findNcmFunctionByCtrl(config, 9).has_value(),
           "pinning a non-existent control interface fails rather than falling back");
}

// A USB string descriptor: bLength, 0x03, then UTF-16LE.
std::vector<uint8_t> stringDescriptor(const std::string& ascii)
{
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(2 + ascii.size() * 2));
    out.push_back(0x03);
    for (const char c : ascii)
    {
        out.push_back(static_cast<uint8_t>(c));
        out.push_back(0);
    }
    return out;
}

void testMacDecoding()
{
    expectEq(apple_usb::macFromStringDescriptor(stringDescriptor("AABBCCDDEEFF")),
             std::string("aa:bb:cc:dd:ee:ff"), "uppercase hex decodes and lowercases");
    expectEq(apple_usb::macFromStringDescriptor(stringDescriptor("0011223344ff")),
             std::string("00:11:22:33:44:ff"), "lowercase hex decodes");

    expect(apple_usb::macFromStringDescriptor({}).empty(), "empty descriptor rejected");
    expect(apple_usb::macFromStringDescriptor({0x04, 0x01, 0x41, 0x00}).empty(),
           "wrong bDescriptorType rejected");
    expect(apple_usb::macFromStringDescriptor(stringDescriptor("AABBCC")).empty(),
           "short MAC rejected rather than padded");
    expect(apple_usb::macFromStringDescriptor(stringDescriptor("AABBCCDDEEFFAA")).empty(),
           "overlong MAC rejected");
    expect(apple_usb::macFromStringDescriptor(stringDescriptor("ZZBBCCDDEEFF")).empty(),
           "non-hex characters rejected");

    // bLength larger than the bytes actually returned must not over-read.
    auto truncated = stringDescriptor("AABBCCDDEEFF");
    truncated.resize(10);
    expect(apple_usb::macFromStringDescriptor(truncated).empty(),
           "truncated transfer rejected rather than over-read");
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    testFindsBothFunctionsInOrder();
    testResolvesEndpointsFromDataAltSetting();
    testExtractsMacStringIndex();
    testMacStringIndexEdgeCases();
    testRejectsUnpairedControlInterface();
    testMissingDataAltSettingLeavesBulkUnset();
    testFindByCtrlInterface();
    testMacDecoding();

    if (failures == 0)
    {
        SPDLOG_INFO("all NCM discovery tests passed");
        return 0;
    }
    SPDLOG_ERROR("{} NCM discovery test(s) failed", failures);
    return 1;
}
