// SPDX-License-Identifier: GPL-3.0-or-later
//
// When to hand the screen between the phone and the car.
//
// The dashboard says whether its CarPlay widget is on screen (CarPlayVisibility,
// once a second and on change); the phone says who owns the screen
// (modesChanged). This decides what to tell the phone, and what to tell the
// dashboard, from those two -- and nothing else, so every rule is testable with
// no phone and no bus.
//
// The messages it asks for are unconfirmed on hardware (airplay/screen_modes.h),
// which is why a disabled handover -- the default -- asks for nothing at all.
#ifndef CARPLAY_SCREEN_HANDOVER_H_
#define CARPLAY_SCREEN_HANDOVER_H_

#include "airplay/screen_modes.h"

#include <chrono>
#include <optional>
#include <vector>

namespace carplay
{

class ScreenHandover
{
  public:
    using clock = std::chrono::steady_clock;

    struct Config
    {
        bool enabled = false;
        // Also send requestUI when giving the screen back, in case untake alone
        // does not bring CarPlay's UI forward on the phone. Hardware decides.
        bool request_ui_on_show = false;
        // Silence from the dashboard this long counts as "CarPlay is visible", so
        // a dashboard that exits hands the screen back instead of stranding it.
        std::chrono::milliseconds visibility_stale_after{3000};
    };

    enum class Command
    {
        take_screen,
        untake_screen,
        request_ui,
        request_keyframe,
    };

    // For the dashboard, published as CarPlayUiEvent.
    enum class Event
    {
        // The phone took the screen back while CarPlay was hidden: show it.
        screen_requested,
        // The phone handed a borrowed screen back while CarPlay was still hidden.
        screen_released,
    };

    struct Actions
    {
        std::vector<Command> commands;
        std::vector<Event> events;
        bool empty() const { return commands.empty() && events.empty(); }
    };

    explicit ScreenHandover(Config config) : config_(config) {}

    Actions onVisibility(bool visible, clock::time_point now);
    Actions onRecording(bool recording);
    Actions onScreenOwner(airplay::ScreenEntity owner);
    Actions tick(clock::time_point now);

    // The car has asked for the screen and not given it back.
    bool carHoldsScreen() const { return held_; }
    // What the node is acting on: the last report, or visible once it is stale.
    bool dashboardShowsCarPlay() const { return visible_; }

  private:
    // Brings what the car holds in line with what the dashboard shows.
    Actions reconcile();

    Config config_;
    bool recording_ = false;

    // Visible until told otherwise: that is the state with nothing to undo.
    bool visible_ = true;
    bool heard_ = false;
    clock::time_point last_heard_{};

    bool held_ = false;
    // A take has been sent and the phone has not yet reported the result. The
    // phone's "accessory owns it now" is the acknowledgement, not an event.
    bool awaiting_take_ack_ = false;
    // The phone took the screen back while CarPlay was hidden. Taking it again
    // on the next heartbeat would be a fight; wait until CarPlay has been shown.
    bool yielded_ = false;
    std::optional<airplay::ScreenEntity> owner_;
};

}  // namespace carplay

#endif  // CARPLAY_SCREEN_HANDOVER_H_
