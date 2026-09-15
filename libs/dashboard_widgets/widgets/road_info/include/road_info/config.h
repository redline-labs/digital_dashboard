#ifndef ROAD_INFO_CONFIG_H
#define ROAD_INFO_CONFIG_H

#include <cstdint>
#include <string>

#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

// What road we are on, from nodes/map_match.
//
// NO uint8_t ANYWHERE IN HERE. yaml-cpp treats `unsigned char` as a CHARACTER
// type: a font size of 14 is written as the unprintable byte 0x0E and read back
// as a bad conversion, which throws out of the YAML decoder and takes the whole
// layout with it. Nothing in the type says so and it fails at load time.
REFLECT_STRUCT(RoadInfoConfig_t,
    (std::string, horizon_zenoh_key, "nodes/map_match/horizon",
        "Horizon Zenoh Key", "Topic carrying the electronic horizon from nodes/map_match"),
    (pub_sub::schema_type_t, horizon_schema_type, pub_sub::schema_type_t::MapHorizon,
        "Horizon Schema Type", "Schema of the horizon topic"),

    (bool, show_name, true,
        "Show Name", "Display the road name"),
    (bool, show_ref, true,
        "Show Ref", "Display the route number, e.g. I-405"),
    (bool, show_speed, true,
        "Show Speed Limit", "Display the posted speed limit"),

    // Off by default: it is a diagnostic, and a driver has no use for it.
    (bool, show_confidence, false,
        "Show Confidence", "Display how sure the matcher is, and the position sigma it used"),

    (std::string, font, "Arial",
        "Font Family", "Font family name"),
    (uint16_t, name_font_size, 22,
        "Name Font Size", "Size of the road name, in points"),
    (uint16_t, detail_font_size, 13,
        "Detail Font Size", "Size of the ref and speed limit, in points"),

    (helpers::Color, text_color, "#FFFFFF",
        "Text Color", "Colour of the road name"),
    (helpers::Color, detail_color, "#9E9E9E",
        "Detail Color", "Colour of the ref and the speed limit"),
    (helpers::Color, background_color, "#00000000",
        "Background Color", "Panel background; fully transparent by default"),

    // A speed limit that is not a posted one must not be shown as if it were.
    // The matcher already refuses to send one, and this is the second line of
    // that defence -- see map_common.capnp's MapSpeedSource.
    (std::string, no_limit_text, "--",
        "No Limit Text", "Shown when OSM records no posted limit for this road"),
    (std::string, no_road_text, "No road",
        "No Road Text", "Shown when the matcher has a fix but no road under it"),
    (std::string, no_fix_text, "Waiting for position",
        "No Fix Text", "Shown before the first horizon arrives"),

    (bool, speed_in_mph, true,
        "Speed in mph", "Convert the posted limit from km/h to mph for display")
)

#endif // ROAD_INFO_CONFIG_H
