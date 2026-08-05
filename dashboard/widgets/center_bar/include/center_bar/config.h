#ifndef CENTER_BAR_CONFIG_H
#define CENTER_BAR_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

// A horizontal bar whose origin is the middle, not the left edge: the marker
// sits at the centre for zero and travels either way from there. This is the
// MoTeC gain/loss strip -- how far ahead or behind the reference lap you are --
// and anything else that is naturally signed around a target.
REFLECT_STRUCT(CenterBarConfig_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed,
        "Schema Type", "Data schema type for the subscription"),
    (std::string, value_expression, "",
        "Value Expression", "Expression to compute the signed value"),

    // Full-scale deflection either side of centre, in the value's own units.
    (float, range, 1.0,
        "Range", "Full-scale deflection either side of centre"),

    (std::string, left_label, "LOSS",
        "Left Label", "Label at the left end of the bar"),
    (std::string, right_label, "GAIN",
        "Right Label", "Label at the right end of the bar"),

    // Which direction counts as good. On a gain/loss strip the useful value is
    // negative -- you are under the reference lap -- so the good end is the left
    // one, and this flips which colour the marker takes.
    (bool, negative_is_good, true,
        "Negative Is Good", "Colour negative values with good_color"),

    (helpers::Color, track_color, "#333333",
        "Track Color", "Colour of the unfilled bar"),
    (helpers::Color, good_color, "#39B54A",
        "Good Color", "Marker colour on the good side"),
    (helpers::Color, bad_color, "#C4281E",
        "Bad Color", "Marker colour on the bad side"),
    (helpers::Color, label_color, "#AAAAAA",
        "Label Color", "Colour of the end labels"),
    (helpers::Color, tick_color, "#777777",
        "Tick Color", "Colour of the centre tick")
)

// `range` is the divisor that turns a reading into a position along the bar.
// Zero divided; a negative range put the marker on the wrong side of centre.
inline std::vector<std::string> validate(CenterBarConfig_t& cfg)
{
    std::vector<std::string> notes;
    if (!(cfg.range > 0.0f))
    {
        notes.push_back("range was " + std::to_string(cfg.range) +
                        ", which is not a scale the marker can be placed against; set to 1");
        cfg.range = 1.0f;
    }
    return notes;
}

#endif // CENTER_BAR_CONFIG_H
