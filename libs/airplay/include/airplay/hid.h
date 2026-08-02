// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HID devices the accessory presents to the phone, and the reports it sends
// on them.
//
// CarPlay input is HID and nothing else: the accessory declares its input
// devices in GET /info's `hidDevices`, and every press, turn and touch is a HID
// input report pushed to the phone in a `hidSendReport` command on the event
// channel. Four devices cover what a head unit has:
//
//   - a touchscreen digitizer (two contacts, absolute coordinates),
//   - a rotary controller: select/home/back, a pointer, and a detent wheel,
//   - consumer media keys (play, pause, next, previous, ...),
//   - a telephony keypad (answer, hang up, mute, DTMF).
//
// The phone tells them apart by the `uuid` on the report, which is why each has
// a fixed id here. Descriptors and reports are pure data, so they are testable
// without a phone (test_hid.cpp).
#ifndef AIRPLAY_HID_H_
#define AIRPLAY_HID_H_

#include "plist/value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace airplay::hid
{

using Bytes = std::vector<uint8_t>;

// Device ids, hex-formatted into the command's `uuid` field. Arbitrary but
// stable: the phone only needs them to be distinct and to match /info.
constexpr uint32_t kTouchUid = 0x2A2A2A2A;
constexpr uint32_t kKnobUid = 0x2A2A2A2B;
constexpr uint32_t kMediaUid = 0x2A2A2A2C;
constexpr uint32_t kTelephonyUid = 0x2A2A2A2D;

// Simultaneous touch contacts. Two is what pinch and rotate need.
constexpr int kTouchContacts = 2;

std::string uidToString(uint32_t uid);

// --- Report descriptors --------------------------------------------------

// The touchscreen's descriptor is sized to the display, since it reports
// absolute coordinates. The phone will not treat a display as touch-capable
// unless a HID device backs the primaryInputDevice advertised for it.
Bytes touchDescriptor(uint32_t x_max, uint32_t y_max);
Bytes knobDescriptor();
Bytes mediaDescriptor();
Bytes telephonyDescriptor();

// --- /info entries -------------------------------------------------------

// One `hidDevices` entry. `display_uuid` ties the device to the display it
// drives, which is what lets the phone route a knob turn to the right screen.
plist::Value touchDevice(uint32_t x_max, uint32_t y_max, const std::string& display_uuid);
plist::Value knobDevice(const std::string& display_uuid);
plist::Value mediaDevice(const std::string& display_uuid);
plist::Value telephonyDevice(const std::string& display_uuid);

// --- Input reports -------------------------------------------------------

struct Contact
{
    uint16_t x = 0;
    uint16_t y = 0;
    bool down = false;
};

// Always kTouchContacts fingers wide, whatever is passed: the descriptor fixes
// the report length, and a short report is discarded rather than misread.
Bytes touchReport(const std::vector<Contact>& contacts);

// The rotary controller's state. Buttons are levels (held until reported
// released); the pointer and wheel are relative deltas, so a report carrying
// only a wheel value is one detent and needs no matching release.
struct KnobState
{
    bool select = false;
    bool home = false;
    bool back = false;
    int pan_x = 0;   // -127..127, clamped
    int pan_y = 0;
    int wheel = 0;   // detents, positive clockwise
};

Bytes knobReport(const KnobState& state);

// Consumer keys. The report is a usage *index* into the descriptor's array, so
// a press is the index followed by 0 to release.
enum class MediaKey : uint8_t
{
    None = 0,
    Play = 1,
    Pause = 2,
    PlayPause = 3,
    Next = 4,
    Previous = 5,
    NavigationGuidance = 6,
};

// Telephony keypad, same array-index encoding as the media keys. HookSwitch is
// answer/hang-up, Drop ends a call, and Key0..Pound are DTMF.
enum class TelephonyKey : uint8_t
{
    None = 0,
    HookSwitch = 1,
    Flash = 2,
    Drop = 3,
    Mute = 4,
    Key0 = 5,
    Key1 = 6,
    Key2 = 7,
    Key3 = 8,
    Key4 = 9,
    Key5 = 10,
    Key6 = 11,
    Key7 = 12,
    Key8 = 13,
    Key9 = 14,
    Star = 15,
    Pound = 16,
    Delete = 17,
};

// True when the value names a key the descriptor declares. Reports for
// anything else would be dropped by the phone with no diagnostic.
bool isKnownMediaKey(uint16_t code);
bool isKnownTelephonyKey(uint16_t code);

Bytes mediaReport(MediaKey key);
Bytes telephonyReport(TelephonyKey key);

// Wraps a report in the `hidSendReport` command body the event channel carries.
Bytes sendReportCommand(uint32_t uid, const Bytes& report);

}  // namespace airplay::hid

#endif  // AIRPLAY_HID_H_
