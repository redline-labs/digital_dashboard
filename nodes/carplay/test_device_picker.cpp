// SPDX-License-Identifier: GPL-3.0-or-later
//
// DevicePicker: with more than one Apple device attached, the one brought up is
// a phone, and not the same failing one every time.
#include "device_picker.h"

#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

apple_usb::DeviceInfo device(uint8_t port, uint8_t active, uint8_t configurations)
{
    apple_usb::DeviceInfo info;
    info.port.bus = 1;
    info.port.ports = {port};
    info.vid = apple_usb::kAppleVendorId;
    info.active_configuration = active;
    info.num_configurations = configurations;
    return info;
}

uint8_t firstPort(const std::vector<apple_usb::DeviceInfo>& order)
{
    return order.empty() ? 0 : order.front().port.ports.front();
}

}  // namespace

int main()
{
    carplay::DevicePicker picker;
    const auto keyboard = device(1, 1, 1);
    const auto phone_a = device(2, 4, 6);
    const auto phone_b = device(3, 6, 6);  // already in the CarPlay configuration

    expect(picker.order({keyboard}).empty(), "an Apple device with no CarPlay configuration is not a candidate");
    expect(firstPort(picker.order({keyboard, phone_a})) == 2,
           "a keyboard that enumerates first does not stand in front of the phone");
    expect(picker.order({keyboard, phone_a, phone_b}).size() == 2, "both phones are candidates");

    // Phone A keeps failing: phone B is tried next, and A after it.
    picker.failed(phone_a.port);
    const auto after_one = picker.order({phone_a, phone_b});
    expect(firstPort(after_one) == 3 && after_one.size() == 2,
           "after a failure the other phone goes first, and the failing one stays in the list");

    // A lone failing phone is still tried.
    picker.failed(phone_a.port);
    expect(firstPort(picker.order({phone_a})) == 2, "a lone phone that keeps failing is still tried");

    // Unplugging forgets its count; so does a success.
    picker.order({phone_b});
    expect(picker.failuresOf(phone_a) == 0, "an unplugged port's failures are forgotten");
    picker.failed(phone_b.port);
    picker.succeeded(phone_b.port);
    expect(picker.failuresOf(phone_b) == 0, "a success clears the count");
    expect(firstPort(picker.order({phone_a, phone_b})) == 2, "and enumeration order decides a tie");

    if (failures == 0)
    {
        std::printf("all device picker checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
