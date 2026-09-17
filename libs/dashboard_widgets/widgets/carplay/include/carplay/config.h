#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "dashboard/page_command.h"
#include "reflection/reflection.h"

// A way off the CarPlay page with no phone connected. The phone's own
// manufacturer tile is the normal way back to the vehicle's screens, and there
// is no phone to draw it -- so without this, a page_stack switched to CarPlay
// with nothing plugged in has no way out but a hardware button.
REFLECT_STRUCT(CarplayReturnButton_t,
    (bool, enabled, false,
        "Enabled", "Show a button to leave CarPlay while no phone session is live"),
    (std::string, label, "Vehicle",
        "Label", "Text on the button"),
    (page_command_t, command, page_command_t{},
        "Command", "What the button does: usually go_to the vehicle page, or back"),
    (uint16_t, width, 200,
        "Width", "Button width in pixels"),
    (uint16_t, height, 56,
        "Height", "Button height in pixels")
)

// Configuration for the CarPlay widget. The widget is a thin client of the
// carplay driver node (nodes/carplay), which owns the USB/iAP2/AirPlay
// session with the phone. These keys must match the driver's configuration.
REFLECT_STRUCT(CarplayConfig_t,
    (std::string, video_key,   "nodes/carplay/video",
        "Video Key", "Zenoh key the driver publishes the phone's H.264/H.265 screen on"),
    (std::string, audio_key,   "nodes/carplay/audio",
        "Audio Key", "Zenoh key the driver publishes phone audio on"),
    (std::string, mic_key,     "nodes/carplay/mic",
        "Microphone Key", "Zenoh key this widget publishes captured microphone audio on, for Siri and calls"),
    (std::string, input_key,   "nodes/carplay/input",
        "Input Key", "Zenoh key this widget publishes touch events to, to send them to the phone"),
    (std::string, session_key, "nodes/carplay/session",
        "Session Key", "Zenoh key carrying session state: whether a phone is connected and what it is doing"),
    (std::string, visibility_key, "nodes/carplay/visibility",
        "Visibility Key", "Zenoh key this widget reports whether it is on screen on, so the driver can hand the screen to the car"),
    (uint32_t, session_stale_after_ms, 3000,
        "Session Stale After (ms)", "No session state for this long means no driver: the return button shows"),
    (CarplayReturnButton_t, return_button, CarplayReturnButton_t{},
        "Return Button", "A button to leave the CarPlay page while no phone is connected")
)

inline std::vector<std::string> validate(CarplayConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampStaleAfter(cfg.session_stale_after_ms, "session_stale_after_ms", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.return_button.width, 40u, 2000u, "return_button.width", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.return_button.height, 24u, 2000u, "return_button.height", notes);
    return notes;
}

#endif // CARPLAY_CONFIG_H
