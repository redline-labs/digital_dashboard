// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "manufacturer button": the tile CarPlay draws on its own home screen,
// supplied by the vehicle manufacturer, which the user presses to hand the
// screen back to the head unit's native UI.
//
// Two halves of one feature, both pure functions so they can be tested without
// a phone (test_oem_button.cpp):
//   - what the accessory advertises in GET /info, and
//   - how the phone's press is recognised on the event channel.
#ifndef AIRPLAY_OEM_BUTTON_H_
#define AIRPLAY_OEM_BUTTON_H_

#include "plist/value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace airplay
{

// One rendition of the button's artwork. CarPlay picks the entry that suits the
// phone's scale factor, so several sizes of the same image may be offered; a
// single one is legal and simply gets scaled.
struct OemIcon
{
    std::vector<uint8_t> png;
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    // False lets CarPlay apply its own icon treatment (corner mask, shine) as
    // it does for app icons. True hands the artwork through untouched.
    bool prerendered = false;
};

struct OemButtonConfig
{
    // When false nothing is advertised at all, which is how CarPlay is told the
    // vehicle has no such button. Note this is decided once, at GET /info time:
    // there is no way to show or hide the button mid-session.
    bool enabled = false;

    // Caption under the icon. CarPlay truncates long labels -- keep it to
    // roughly 13 characters, which is what a head unit's own UI budget allows.
    std::string label;

    std::vector<OemIcon> icons;
};

// Adds the manufacturer-button keys to a GET /info dict. A no-op when the
// button is disabled: omitting the keys is what says "this vehicle has no such
// button", which is not the same as sending oemIconVisible = false.
void addOemButtonInfo(const OemButtonConfig& config, plist::Value& info);

// True when `command` -- one decoded POST /command body from the event channel
// -- is the phone reporting a manufacturer-button press.
//
// The press arrives as a `requestUI` carrying no url. The same command *with* a
// url is an app asking the head unit to open something specific, so the url is
// the only thing that tells the two apart.
bool isOemButtonPress(const plist::Value& command);

}  // namespace airplay

#endif  // AIRPLAY_OEM_BUTTON_H_
