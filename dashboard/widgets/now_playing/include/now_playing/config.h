#ifndef NOW_PLAYING_CONFIG_H
#define NOW_PLAYING_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "helpers/color.h"
#include "reflection/reflection.h"
#include "config_codec/config_limits.h"

// Now-playing widget: renders media metadata published by the carplay driver
// node. Purely a subscriber -- it works alongside (or entirely without) the
// CarPlay video widget, which is the point of publishing metadata separately.
//
// It also watches the call topic. A call takes the widget over for as long as it
// lasts and hands it back to the music afterwards, which is what the head unit
// in the car does: one panel, and whatever matters most at the time is in it.
REFLECT_STRUCT(NowPlayingConfig_t,
    (std::string, zenoh_key, "nodes/carplay/nowplaying",
        "Zenoh Key", "Zenoh topic publishing CarPlayNowPlaying metadata"),
    (bool, show_album_art, true,
        "Show Album Art", "Draw album artwork when the phone provides it"),
    (bool, show_progress, true,
        "Show Progress", "Draw the track progress bar and elapsed/duration times"),
    (helpers::Color, title_color, "#FFFFFF",
        "Title Color", "Color of the track title"),
    (helpers::Color, detail_color, "#AAAAAA",
        "Detail Color", "Color of the artist/album/app text"),
    (helpers::Color, accent_color, "#FFA500",
        "Accent Color", "Color of the progress bar"),

    // Call takeover.
    (bool, show_calls, true,
        "Show Calls", "Let an active phone call take the widget over"),
    (std::string, call_zenoh_key, "nodes/carplay/call",
        "Call Zenoh Key", "Zenoh topic publishing CarPlayCall state"),
    (helpers::Color, call_accent_color, "#39B54A",
        "Call Accent Color", "Color of the call badge and status text"),
    // How long the cross-fade between the music and the call face runs.
    (uint16_t, transition_ms, 260,
        "Transition (ms)", "Duration of the cross-fade between music and call"),
    // How long "Call ended" stays up after the phone hangs up, before the music
    // comes back. Without it the call face vanishes the instant the call drops
    // and the takeover reads as a glitch rather than a state.
    (uint16_t, call_linger_ms, 1600,
        "Call Linger (ms)", "How long 'Call ended' stays up before the music returns")
)

// transition_ms drives a QVariantAnimation and call_linger_ms a QTimer. Zero on
// either is legal but degenerate -- an instant cut and an invisible "Call ended"
// -- and an unbounded value parks the widget mid-fade or holds the call face up
// long after the call is over.
inline std::vector<std::string> validate(NowPlayingConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto<uint16_t>(cfg.transition_ms, 0u, 2000u, "transition_ms", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.call_linger_ms, 0u, 10000u, "call_linger_ms", notes);
    return notes;
}

#endif // NOW_PLAYING_CONFIG_H
