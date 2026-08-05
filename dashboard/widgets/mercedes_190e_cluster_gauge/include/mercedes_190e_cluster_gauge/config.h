#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>
#include <string>
#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_STRUCT(sub_gauge_config_t,
    (float, min_value, 0.0),
    (float, max_value, 100.0),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, value_expression, "")
)

// One block for the sub-gauge type, not one per sub-gauge: all four use the
// same struct, so the panel shows these labels wherever a sub-gauge is nested.
REFLECT_METADATA(sub_gauge_config_t,
    (min_value, "Minimum Value", "Reading at the empty end of the sweep"),
    (max_value, "Maximum Value", "Reading at the full end of the sweep"),
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (value_expression, "Value Expression", "Expression evaluated against the message to produce the reading")
)

// The bottom sub-gauge on a real 190E cluster is not a tick scale. It is a
// tapered crescent -- thin at the economical end, thick at the uneconomical one
// -- outlined in white with its upper portion filled solid red, and the word
// ECONOMY printed above it. Everything here describes that band; the value it
// reads still comes from bottom_gauge like any other sub-gauge.
REFLECT_STRUCT(economy_sweep_config_t,
    (std::string, label, "ECONOMY"),
    // Where the red section starts, as a fraction of the sweep from the
    // economical end. 0 paints the whole band red, 1 paints none of it.
    (float, red_start_fraction, 0.60),
    (helpers::Color, outline_color, "#FFFFFF"),
    (helpers::Color, red_color, "#C4281E")
)

REFLECT_METADATA(economy_sweep_config_t,
    (label, "Label", "Text printed above the sweep; ECONOMY on a stock cluster"),
    (red_start_fraction, "Red Start", "Fraction along the sweep where the red section begins (0-1)"),
    (outline_color, "Outline Color", "Colour of the band outline and the label"),
    (red_color, "Red Color", "Fill colour of the uneconomical section")
)

REFLECT_STRUCT(Mercedes190EClusterGaugeConfig_t,
    (sub_gauge_config_t, fuel_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, right_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, bottom_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, left_gauge, sub_gauge_config_t{}),
    (economy_sweep_config_t, economy_sweep, economy_sweep_config_t{})
)

REFLECT_METADATA(Mercedes190EClusterGaugeConfig_t,
    (fuel_gauge, "Fuel Gauge", "The sub-gauge in the fuel position"),
    (right_gauge, "Right Gauge", "The sub-gauge on the right of the cluster"),
    (bottom_gauge, "Bottom Gauge", "The sub-gauge along the bottom; what the economy sweep reads"),
    (left_gauge, "Left Gauge", "The sub-gauge on the left of the cluster"),
    (economy_sweep, "Economy Sweep", "The tapered ECONOMY band drawn over the bottom gauge")
)

// Each sub-gauge clamps incoming readings to its own min/max. Those come
// straight from YAML with nothing checking their order, and std::clamp's
// precondition is !(max < min) -- an inverted pair was undefined behaviour, four
// times over.
inline std::vector<std::string> validate(Mercedes190EClusterGaugeConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::orderRange(cfg.fuel_gauge.min_value, cfg.fuel_gauge.max_value,
                                  "fuel_gauge", notes);
    dashboard::limits::orderRange(cfg.right_gauge.min_value, cfg.right_gauge.max_value,
                                  "right_gauge", notes);
    dashboard::limits::orderRange(cfg.bottom_gauge.min_value, cfg.bottom_gauge.max_value,
                                  "bottom_gauge", notes);
    dashboard::limits::orderRange(cfg.left_gauge.min_value, cfg.left_gauge.max_value,
                                  "left_gauge", notes);
    // Drives where along the band the red fill starts. Outside [0, 1] it either
    // runs backwards off the band or paints past its end.
    dashboard::limits::clampInto(cfg.economy_sweep.red_start_fraction, 0.0f, 1.0f,
                                 "economy_sweep.red_start_fraction", notes);
    return notes;
}

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H