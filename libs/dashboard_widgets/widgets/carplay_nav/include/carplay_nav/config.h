#ifndef CARPLAY_NAV_CONFIG_H
#define CARPLAY_NAV_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "helpers/color.h"
#include "reflection/reflection.h"
#include "config_codec/config_limits.h"

// Turn-by-turn guidance from CarPlay. Like now_playing, this is purely a
// subscriber to the driver node's metadata topic -- it needs no USB, no AirPlay
// and no video surface, which is the whole point of the node publishing route
// guidance separately from the projected screen.
REFLECT_STRUCT(CarPlayNavConfig_t,
    (std::string, zenoh_key, "nodes/carplay/nav",
        "Zenoh Key", "Zenoh topic publishing CarPlayNav guidance"),
    (bool, imperial_units, false,
        "Imperial Units", "Show feet and miles instead of metres and kilometres"),
    // Draw the trip summary strip (remaining distance, remaining time, ETA).
    (bool, show_trip_summary, true,
        "Show Trip Summary", "Draw remaining distance, remaining time and ETA"),
    (helpers::Color, arrow_color, "#39B54A",
        "Arrow Color", "Color of the maneuver arrow"),
    (helpers::Color, distance_color, "#FFFFFF",
        "Distance Color", "Color of the distance-to-maneuver text"),
    (helpers::Color, road_color, "#FFFFFF",
        "Road Color", "Color of the road name being turned onto"),
    (helpers::Color, detail_color, "#AAAAAA",
        "Detail Color", "Color of the secondary text and trip summary"),
    (helpers::Color, background_color, "#00000000",
        "Background Color", "Fill behind the card; default is transparent"),
    // Shown in place of the turn card when the phone reports no active route.
    (std::string, idle_text, "No route",
        "Idle Text", "Shown when there is no active route")
)

// Nothing here is a divisor or a loop bound, so there is no range to clamp. The
// one thing worth refusing is an empty key: a subscriber declared on "" never
// matches anything, and the widget would sit on the idle face forever with
// nothing said about why.
inline std::vector<std::string> validate(CarPlayNavConfig_t& cfg)
{
    std::vector<std::string> notes;
    if (cfg.zenoh_key.empty())
    {
        cfg.zenoh_key = "nodes/carplay/nav";
        notes.emplace_back("zenoh_key was empty; reset to the driver node's default nav topic");
    }
    return notes;
}

#endif // CARPLAY_NAV_CONFIG_H
