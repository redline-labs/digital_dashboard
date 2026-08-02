// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/hid.h"

#include "plist/binary.h"

#include <algorithm>

namespace airplay::hid
{
namespace
{

// Report descriptor item bytes are written literally rather than through a
// builder: a descriptor is read against the HID spec far more often than it is
// edited, and the usage comments only line up if the bytes are in source order.

int8_t clampAxis(int value)
{
    return static_cast<int8_t>(std::clamp(value, -127, 127));
}

plist::Value deviceEntry(uint32_t uid, const std::string& name, const Bytes& descriptor,
                         const std::string& display_uuid)
{
    plist::Value device = plist::Value::dict();
    device.set("hidProductID", plist::Value::integer(1));
    device.set("hidVendorID", plist::Value::integer(2));
    device.set("hidCountryCode", plist::Value::integer(0));
    device.set("uuid", plist::Value::string(uidToString(uid)));
    device.set("name", plist::Value::string(name));
    device.set("displayUUID", plist::Value::string(display_uuid));
    device.set("hidDescriptor", plist::Value::data(descriptor));
    return device;
}

}  // namespace

std::string uidToString(uint32_t uid)
{
    // Lower-case hex with no padding, which is how the /info entry and the
    // report's uuid must agree -- the phone matches them as strings.
    static constexpr char kDigits[] = "0123456789abcdef";
    if (uid == 0)
    {
        return "0";
    }
    std::string out;
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        const uint8_t nibble = (uid >> shift) & 0xF;
        if (out.empty() && nibble == 0)
        {
            continue;
        }
        out.push_back(kDigits[nibble]);
    }
    return out;
}

Bytes touchDescriptor(uint32_t x_max, uint32_t y_max)
{
    Bytes out{0x05, 0x0D,   // Usage Page (Digitizers)
              0x09, 0x04,   // Usage (Touch Screen)
              0xA1, 0x01};  // Collection (Application)

    for (int i = 0; i < kTouchContacts; ++i)
    {
        const Bytes finger{
            0x05, 0x0D,                                            // Usage Page (Digitizers)
            0x09, 0x22,                                            // Usage (Finger)
            0xA1, 0x02,                                            // Collection (Logical)
            0x09, 0x38,                                            // Usage (Transducer Index)
            0x75, 0x08,                                            // Report Size (8)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x15, 0x00,                                            // Logical Minimum (0)
            0x25, 0x01,                                            // Logical Maximum (1)
            0x09, 0x33,                                            // Usage (Touch)
            0x75, 0x01,                                            // Report Size (1)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x95, 0x07,                                            // Report Count (7)
            0x81, 0x03,                                            // Input (Cnst,Var,Abs) padding
            0x05, 0x01,                                            // Usage Page (Generic Desktop)
            0x26, static_cast<uint8_t>(x_max & 0xFF),
            static_cast<uint8_t>((x_max >> 8) & 0xFF),             // Logical Maximum (xMax)
            0x09, 0x30,                                            // Usage (X)
            0x75, 0x10,                                            // Report Size (16)
            0x95, 0x01,                                            // Report Count (1)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0x26, static_cast<uint8_t>(y_max & 0xFF),
            static_cast<uint8_t>((y_max >> 8) & 0xFF),             // Logical Maximum (yMax)
            0x09, 0x31,                                            // Usage (Y)
            0x81, 0x02,                                            // Input (Data,Var,Abs)
            0xC0};                                                 // End Collection
        out.insert(out.end(), finger.begin(), finger.end());
    }
    out.push_back(0xC0);  // End Collection
    return out;
}

Bytes knobDescriptor()
{
    // A MultiAxisController: three buttons in the first byte, then a relative
    // pointer, then the detent wheel. Home and Back are Consumer usages rather
    // than plain buttons so the phone knows what they mean; a generic button
    // would be delivered to the foreground app instead of navigating.
    return Bytes{
        0x05, 0x01,        // Usage Page (Generic Desktop)
        0x09, 0x08,        // Usage (MultiAxisController)
        0xA1, 0x01,        // Collection (Application)
        0x05, 0x09,        //   Usage Page (Button)
        0x09, 0x01,        //   Usage (Button 1 -- Select)
        0x15, 0x00,        //   Logical Minimum (0)
        0x25, 0x01,        //   Logical Maximum (1)
        0x75, 0x01,        //   Report Size (1)
        0x95, 0x01,        //   Report Count (1)
        0x81, 0x02,        //   Input (Data,Var,Abs)   bit 0: Select
        0x05, 0x0C,        //   Usage Page (Consumer)
        0x0A, 0x23, 0x02,  //   Usage (AC Home)
        0x0A, 0x24, 0x02,  //   Usage (AC Back)
        0x95, 0x02,        //   Report Count (2)
        0x81, 0x02,        //   Input (Data,Var,Abs)   bits 1-2: Home, Back
        0x95, 0x05,        //   Report Count (5)
        0x81, 0x01,        //   Input (Constant)       bits 3-7: padding
        0x05, 0x01,        //   Usage Page (Generic Desktop)
        0x09, 0x01,        //   Usage (Pointer)
        0xA1, 0x00,        //   Collection (Physical)
        0x09, 0x30,        //     Usage (X)
        0x09, 0x31,        //     Usage (Y)
        0x15, 0x81,        //     Logical Minimum (-127)
        0x25, 0x7F,        //     Logical Maximum (127)
        0x75, 0x08,        //     Report Size (8)
        0x95, 0x02,        //     Report Count (2)
        0x81, 0x02,        //     Input (Data,Var,Abs)
        0xC0,              //   End Collection
        0x09, 0x38,        //   Usage (Wheel)
        0x15, 0x81,        //   Logical Minimum (-127)
        0x25, 0x7F,        //   Logical Maximum (127)
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x01,        //   Report Count (1)
        0x81, 0x06,        //   Input (Data,Var,Rel)
        0xC0};             // End Collection
}

Bytes mediaDescriptor()
{
    // An array-style report: one byte carrying the index of the usage pressed,
    // 0 for none. Logical Maximum has to match the usage count exactly or the
    // trailing usages are unreachable.
    return Bytes{
        0x05, 0x0C,        // Usage Page (Consumer)
        0x09, 0x01,        // Usage (Consumer Control)
        0xA1, 0x01,        // Collection (Application)
        0x15, 0x00,        //   Logical Minimum (0)
        0x25, 0x06,        //   Logical Maximum (6)
        0x05, 0x0C,        //   Usage Page (Consumer)
        0x0A, 0x00, 0x00,  //   Usage (Unassigned)          index 0
        0x0A, 0xB0, 0x00,  //   Usage (Play)                index 1
        0x0A, 0xB1, 0x00,  //   Usage (Pause)               index 2
        0x0A, 0xCD, 0x00,  //   Usage (Play/Pause)          index 3
        0x0A, 0xB5, 0x00,  //   Usage (Scan Next Track)     index 4
        0x0A, 0xB6, 0x00,  //   Usage (Scan Previous Track) index 5
        0x0A, 0x9E, 0x02,  //   Usage (AC Navigation Guidance) index 6
        0x75, 0x08,        //   Report Size (8)
        0x95, 0x01,        //   Report Count (1)
        0x81, 0x00,        //   Input (Data,Array,Abs)
        0xC0};             // End Collection
}

Bytes telephonyDescriptor()
{
    return Bytes{
        0x05, 0x0B,  // Usage Page (Telephony)
        0x09, 0x07,  // Usage (Telephony Keypad)
        0xA1, 0x01,  // Collection (Application)
        0x15, 0x00,  //   Logical Minimum (0)
        0x25, 0x11,  //   Logical Maximum (17)
        0x05, 0x0B,  //   Usage Page (Telephony)
        0x09, 0x00,  //   Usage (Unassigned)     index 0
        0x09, 0x20,  //   Usage (Hook Switch)    index 1
        0x09, 0x21,  //   Usage (Flash)          index 2
        0x09, 0x26,  //   Usage (Drop)           index 3
        0x09, 0x2F,  //   Usage (Mute)           index 4
        0x09, 0xB0,  //   Usage (Phone Key 0)    index 5
        0x09, 0xB1,  //   Usage (Phone Key 1)    index 6
        0x09, 0xB2,  //   Usage (Phone Key 2)    index 7
        0x09, 0xB3,  //   Usage (Phone Key 3)    index 8
        0x09, 0xB4,  //   Usage (Phone Key 4)    index 9
        0x09, 0xB5,  //   Usage (Phone Key 5)    index 10
        0x09, 0xB6,  //   Usage (Phone Key 6)    index 11
        0x09, 0xB7,  //   Usage (Phone Key 7)    index 12
        0x09, 0xB8,  //   Usage (Phone Key 8)    index 13
        0x09, 0xB9,  //   Usage (Phone Key 9)    index 14
        0x09, 0xBA,  //   Usage (Phone Key Star) index 15
        0x09, 0xBB,  //   Usage (Phone Key Pound) index 16
        0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
        0x09, 0x2A,  //   Usage (Keyboard DELETE) index 17
        0x75, 0x08,  //   Report Size (8)
        0x95, 0x01,  //   Report Count (1)
        0x81, 0x00,  //   Input (Data,Array,Abs)
        0xC0};       // End Collection
}

plist::Value touchDevice(uint32_t x_max, uint32_t y_max, const std::string& display_uuid)
{
    return deviceEntry(kTouchUid, "Dashboard Touchscreen", touchDescriptor(x_max, y_max),
                       display_uuid);
}

plist::Value knobDevice(const std::string& display_uuid)
{
    return deviceEntry(kKnobUid, "Dashboard Controller", knobDescriptor(), display_uuid);
}

plist::Value mediaDevice(const std::string& display_uuid)
{
    return deviceEntry(kMediaUid, "Dashboard Media Keys", mediaDescriptor(), display_uuid);
}

plist::Value telephonyDevice(const std::string& display_uuid)
{
    return deviceEntry(kTelephonyUid, "Dashboard Telephony", telephonyDescriptor(), display_uuid);
}

Bytes touchReport(const std::vector<Contact>& contacts)
{
    constexpr int kBytesPerFinger = 6;  // index, touch(+pad), X(16 LE), Y(16 LE)
    Bytes report(kBytesPerFinger * kTouchContacts, 0);

    for (int i = 0; i < kTouchContacts; ++i)
    {
        // The transducer index is the finger's fixed slot, and is reported even
        // for a slot with no contact: the phone tracks a finger by the slot it
        // consistently appears in.
        uint8_t* finger = report.data() + (i * kBytesPerFinger);
        finger[0] = static_cast<uint8_t>(i);
        if (static_cast<size_t>(i) >= contacts.size())
        {
            continue;
        }
        const Contact& contact = contacts[static_cast<size_t>(i)];
        finger[1] = contact.down ? 0x01 : 0x00;
        finger[2] = static_cast<uint8_t>(contact.x & 0xFF);
        finger[3] = static_cast<uint8_t>((contact.x >> 8) & 0xFF);
        finger[4] = static_cast<uint8_t>(contact.y & 0xFF);
        finger[5] = static_cast<uint8_t>((contact.y >> 8) & 0xFF);
    }
    return report;
}

Bytes knobReport(const KnobState& state)
{
    Bytes report(4, 0);
    report[0] = static_cast<uint8_t>((state.select ? 0x01 : 0) | (state.home ? 0x02 : 0) |
                                     (state.back ? 0x04 : 0));
    report[1] = static_cast<uint8_t>(clampAxis(state.pan_x));
    report[2] = static_cast<uint8_t>(clampAxis(state.pan_y));
    report[3] = static_cast<uint8_t>(clampAxis(state.wheel));
    return report;
}

bool isKnownMediaKey(uint16_t code)
{
    return code <= static_cast<uint16_t>(MediaKey::NavigationGuidance);
}

bool isKnownTelephonyKey(uint16_t code)
{
    return code <= static_cast<uint16_t>(TelephonyKey::Delete);
}

Bytes mediaReport(MediaKey key)
{
    return Bytes{static_cast<uint8_t>(key)};
}

Bytes telephonyReport(TelephonyKey key)
{
    return Bytes{static_cast<uint8_t>(key)};
}

Bytes sendReportCommand(uint32_t uid, const Bytes& report)
{
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("hidSendReport"));
    command.set("uuid", plist::Value::string(uidToString(uid)));
    command.set("hidReport", plist::Value::data(report));
    return plist::encodeBinary(command);
}

}  // namespace airplay::hid
