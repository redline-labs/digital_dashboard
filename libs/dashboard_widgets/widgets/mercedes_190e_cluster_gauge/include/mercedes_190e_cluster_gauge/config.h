#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>
#include <string>
#include "helpers/color.h"
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "pub_sub/subscription.h"
#include "config_codec/config_limits.h"

REFLECT_STRUCT(sub_gauge_config_t,
    (float, min_value, 0.0,
        "Minimum Value", "Reading at the empty end of the sweep"),
    (float, max_value, 100.0,
        "Maximum Value", "Reading at the full end of the sweep"),
    (pub_sub::subscription_t, source, pub_sub::subscription_t{},
        "Source", "The reading: topic, schema, expression and loss-of-comm timeout")
)

// The bottom sub-gauge on a real 190E cluster is not a tick scale. It is a
// tapered crescent -- thin at the economical end, thick at the uneconomical one
// -- outlined in white with its upper portion filled solid red, and the word
// ECONOMY printed above it. Everything here describes that band; the value it
// reads still comes from bottom_gauge like any other sub-gauge.
REFLECT_STRUCT(economy_sweep_config_t,
    (std::string, label, "ECONOMY",
        "Label", "Text printed above the sweep; ECONOMY on a stock cluster"),
    // Where the red section starts, as a fraction of the sweep from the
    // economical end. 0 paints the whole band red, 1 paints none of it.
    (float, red_start_fraction, 0.60f,
        "Red Start", "Fraction along the sweep where the red section begins (0-1)"),
    (helpers::Color, outline_color, "#FFFFFF",
        "Outline Color", "Colour of the band outline and the label"),
    (helpers::Color, red_color, "#C4281E",
        "Red Color", "Fill colour of the uneconomical section")
)

REFLECT_STRUCT(Mercedes190EClusterGaugeConfig_t,
    (sub_gauge_config_t, fuel_gauge, sub_gauge_config_t{},
        "Fuel Gauge", "The sub-gauge in the fuel position"),
    (sub_gauge_config_t, right_gauge, sub_gauge_config_t{},
        "Right Gauge", "The sub-gauge on the right of the cluster"),
    (sub_gauge_config_t, bottom_gauge, sub_gauge_config_t{},
        "Bottom Gauge", "The sub-gauge along the bottom; what the economy sweep reads"),
    (sub_gauge_config_t, left_gauge, sub_gauge_config_t{},
        "Left Gauge", "The sub-gauge on the left of the cluster"),
    (economy_sweep_config_t, economy_sweep, economy_sweep_config_t{},
        "Economy Sweep", "The tapered ECONOMY band drawn over the bottom gauge")
)

// Each sub-gauge clamps incoming readings to its own min/max. Those come
// straight from YAML with nothing checking their order, and std::clamp's
// precondition is !(max < min) -- an inverted pair was undefined behaviour, four
// times over. Declared on the sub-gauge, so applyLimits runs it for all four.
inline std::vector<std::string> validate(sub_gauge_config_t& gauge)
{
    std::vector<std::string> notes;
    config_codec::limits::orderRange(gauge.min_value, gauge.max_value, "min_value/max_value", notes);
    return notes;
}

// Drives where along the band the red fill starts. Outside [0, 1] it either
// runs backwards off the band or paints past its end.
inline std::vector<std::string> validate(economy_sweep_config_t& sweep)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto(sweep.red_start_fraction, 0.0f, 1.0f, "red_start_fraction", notes);
    return notes;
}

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H