// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who owns the screen: the messages that hand it between the phone and the car.
//
// UNCONFIRMED ON HARDWARE. Nothing in this tree had sent `changeModes`, or a
// `requestUI` from the car to the phone, before these were written, and LIVI --
// the implementation the rest of the stack was ported from and checked against
// -- never sends either. What each constant rests on is written beside it in
// screen_modes.cpp. The node keeps the whole handover off by default
// (`screen_handover.enabled`) until a phone has accepted them, because a wrong
// resource constant is the kind of mistake that ends a session.
//
// Kept to this one file so that when a capture says otherwise, it is changed in
// one place, with airplay_test_screen_modes pinning the new form.
#ifndef AIRPLAY_SCREEN_MODES_H_
#define AIRPLAY_SCREEN_MODES_H_

#include "airplay/channel_crypto.h"
#include "plist/value.h"

#include <cstdint>
#include <optional>
#include <string>

namespace airplay
{

// Who a resource belongs to, as `modesChanged` reports it.
enum class ScreenEntity : int64_t
{
    none = 0,
    // The phone. CarPlay calls it the controller.
    controller = 1,
    accessory = 2,
};

enum class ScreenTransfer
{
    // The car wants the screen for its own UI.
    take,
    // The car gives back a screen it took.
    untake,
};

// A `changeModes` command body for the main screen, ready for
// EventChannel::queueCommand.
Bytes buildChangeModesCommand(ScreenTransfer transfer);

// A `requestUI` command body: the car asking the phone to show CarPlay, the
// mirror of the phone's own manufacturer-button press. With a url, a specific
// CarPlay app.
Bytes buildRequestUiCommand(const std::optional<std::string>& url);

// The main screen's owner from one decoded `modesChanged` command, or nullopt
// when the command does not say -- which is also the answer for anything
// malformed. A modesChanged about only audio or app states is normal and carries
// no screen entry.
std::optional<ScreenEntity> parseScreenOwner(const plist::Value& modes_changed);

}  // namespace airplay

#endif  // AIRPLAY_SCREEN_MODES_H_
