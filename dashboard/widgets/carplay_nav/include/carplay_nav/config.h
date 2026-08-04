#ifndef CARPLAY_NAV_CONFIG_H
#define CARPLAY_NAV_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "helpers/color.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

// Turn-by-turn guidance from CarPlay. Like now_playing, this is purely a
// subscriber to the driver node's metadata topic -- it needs no USB, no AirPlay
// and no video surface, which is the whole point of the node publishing route
// guidance separately from the projected screen.
REFLECT_STRUCT(CarPlayNavConfig_t,
    (std::string, zenoh_key, "nodes/carplay/nav"),
    (bool, imperial_units, false),
    // Draw the trip summary strip (remaining distance, remaining time, ETA).
    (bool, show_trip_summary, true),
    (helpers::Color, arrow_color, "#39B54A"),
    (helpers::Color, distance_color, "#FFFFFF"),
    (helpers::Color, road_color, "#FFFFFF"),
    (helpers::Color, detail_color, "#AAAAAA"),
    (helpers::Color, background_color, "#00000000"),
    // Shown in place of the turn card when the phone reports no active route.
    (std::string, idle_text, "No route")
)

REFLECT_METADATA(CarPlayNavConfig_t,
    (zenoh_key, "Zenoh Key", "Zenoh topic publishing CarPlayNav guidance"),
    (imperial_units, "Imperial Units", "Show feet and miles instead of metres and kilometres"),
    (show_trip_summary, "Show Trip Summary", "Draw remaining distance, remaining time and ETA"),
    (arrow_color, "Arrow Color", "Color of the maneuver arrow"),
    (distance_color, "Distance Color", "Color of the distance-to-maneuver text"),
    (road_color, "Road Color", "Color of the road name being turned onto"),
    (detail_color, "Detail Color", "Color of the secondary text and trip summary"),
    (background_color, "Background Color", "Fill behind the card; default is transparent"),
    (idle_text, "Idle Text", "Shown when there is no active route")
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
