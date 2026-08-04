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
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, value_expression, ""),

    // Full-scale deflection either side of centre, in the value's own units.
    (float, range, 1.0),

    (std::string, left_label, "LOSS"),
    (std::string, right_label, "GAIN"),

    // Which direction counts as good. On a gain/loss strip the useful value is
    // negative -- you are under the reference lap -- so the good end is the left
    // one, and this flips which colour the marker takes.
    (bool, negative_is_good, true),

    (helpers::Color, track_color, "#333333"),
    (helpers::Color, good_color, "#39B54A"),
    (helpers::Color, bad_color, "#C4281E"),
    (helpers::Color, label_color, "#AAAAAA"),
    (helpers::Color, tick_color, "#777777")
)

REFLECT_METADATA(CenterBarConfig_t,
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (value_expression, "Value Expression", "Expression to compute the signed value"),
    (range, "Range", "Full-scale deflection either side of centre"),
    (left_label, "Left Label", "Label at the left end of the bar"),
    (right_label, "Right Label", "Label at the right end of the bar"),
    (negative_is_good, "Negative Is Good", "Colour negative values with good_color"),
    (track_color, "Track Color", "Colour of the unfilled bar"),
    (good_color, "Good Color", "Marker colour on the good side"),
    (bad_color, "Bad Color", "Marker colour on the bad side"),
    (label_color, "Label Color", "Colour of the end labels"),
    (tick_color, "Tick Color", "Colour of the centre tick")
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
