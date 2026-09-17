// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_handover.h"

#include <spdlog/spdlog.h>

namespace carplay
{

ScreenHandover::Actions ScreenHandover::reconcile()
{
    Actions actions;
    if (!config_.enabled || !recording_)
    {
        return actions;
    }

    const bool want_screen = !visible_ && !yielded_;
    if (want_screen && !held_)
    {
        actions.commands.push_back(Command::take_screen);
        held_ = true;
        awaiting_take_ack_ = true;
    }
    else if (!want_screen && held_)
    {
        actions.commands.push_back(Command::untake_screen);
        if (config_.request_ui_on_show)
        {
            actions.commands.push_back(Command::request_ui);
        }
        // The widget re-subscribing already prompts one, but that path is
        // edge-triggered on the topic; ask directly so the picture is not
        // waiting on it.
        actions.commands.push_back(Command::request_keyframe);
        held_ = false;
        awaiting_take_ack_ = false;
    }
    return actions;
}

ScreenHandover::Actions ScreenHandover::onVisibility(bool visible, clock::time_point now)
{
    heard_ = true;
    last_heard_ = now;
    if (visible == visible_)
    {
        return {};
    }
    visible_ = visible;
    if (visible)
    {
        // CarPlay is on screen again, so a later hide is a fresh request.
        yielded_ = false;
    }
    return reconcile();
}

ScreenHandover::Actions ScreenHandover::onRecording(bool recording)
{
    if (recording)
    {
        if (recording_)
        {
            return {};
        }
        recording_ = true;
        // The dashboard may have hidden CarPlay before the phone was ready.
        return reconcile();
    }

    // The session is gone, and with it everything the phone was told.
    recording_ = false;
    held_ = false;
    awaiting_take_ack_ = false;
    yielded_ = false;
    owner_.reset();
    return {};
}

ScreenHandover::Actions ScreenHandover::onScreenOwner(airplay::ScreenEntity owner)
{
    Actions actions;
    const std::optional<airplay::ScreenEntity> previous = owner_;
    owner_ = owner;
    if (!config_.enabled || !recording_)
    {
        return actions;
    }

    switch (owner)
    {
        case airplay::ScreenEntity::accessory:
            if (awaiting_take_ack_)
            {
                awaiting_take_ack_ = false;
            }
            else if (!visible_ && !held_ && previous == airplay::ScreenEntity::controller)
            {
                // Handed back after Siri or a call while CarPlay stayed hidden:
                // the car holds it again without having asked.
                held_ = true;
                yielded_ = false;
                actions.events.push_back(Event::screen_released);
            }
            break;

        case airplay::ScreenEntity::controller:
            if (awaiting_take_ack_)
            {
                // The phone did not accept the take. Not an event: nobody asked
                // for CarPlay, and bouncing the dashboard back to it would undo
                // the driver's own button press.
                SPDLOG_WARN("[node] the phone kept the screen after changeModes take");
                held_ = false;
                awaiting_take_ack_ = false;
                yielded_ = true;
            }
            else if (held_)
            {
                held_ = false;
                yielded_ = true;
                if (!visible_)
                {
                    actions.events.push_back(Event::screen_requested);
                }
            }
            break;

        case airplay::ScreenEntity::none:
            break;
    }
    return actions;
}

ScreenHandover::Actions ScreenHandover::tick(clock::time_point now)
{
    if (!heard_ || visible_ || now - last_heard_ <= config_.visibility_stale_after)
    {
        return {};
    }
    SPDLOG_WARN("[node] no visibility from the dashboard for {} ms; treating CarPlay as visible",
                config_.visibility_stale_after.count());
    visible_ = true;
    yielded_ = false;
    return reconcile();
}

}  // namespace carplay
