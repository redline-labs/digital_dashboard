#ifndef SPARKLINE_CONFIG_H
#define SPARKLINE_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "helpers/color.h"
#include "config_codec/config_limits.h"

REFLECT_STRUCT(SparklineConfig_t,
    (std::string, units, "Untitled",
        "Units Label", "Units text to display (e.g., mph, °C)"),
    (double, min_value, 0.0,
        "Minimum Value", "Minimum value for the Y-axis scale"),
    (double, max_value, 100.0,
        "Maximum Value", "Maximum value for the Y-axis scale"),
    (helpers::Color, line_color, "#0000FF",
        "Line Color", "Color of the sparkline graph"),
    (helpers::Color, text_color, "#FFFFFF",
        "Text Color", "Color of the value and units text"),
    (std::string, font_family, "Arial",
        "Font Family", "Font family for the displayed text"),
    (uint16_t, font_size_value, 24,
        "Value Font Size", "Font size for the numeric value"),
    (uint16_t, font_size_units, 10,
        "Units Font Size", "Font size for the units label"),
    (uint16_t, update_rate, 30,
        "Update Rate (Hz)", "Graph update rate in Hertz"),
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed,
        "Schema Type", "Data schema type for the subscription"),
    (std::string, value_expression, "",
        "Value Expression", "Expression to extract/compute the value")
)

// update_rate feeds `1000 / update_rate` as a millisecond timer interval, so
// anything above 1000 became a 0 ms timer that fired on every pass of the event
// loop. min == max makes the Y-axis division degenerate, and a point size of
// zero makes Qt complain on every paint.
inline std::vector<std::string> validate(SparklineConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto<uint16_t>(cfg.update_rate, 1u, config_codec::limits::kMaxUpdateRateHz,
                                           "update_rate", notes);
    config_codec::limits::orderRange(cfg.min_value, cfg.max_value, "the value range", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.font_size_value, 1u, 200u, "font_size_value", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.font_size_units, 1u, 200u, "font_size_units", notes);
    return notes;
}

#endif // SPARKLINE_CONFIG_H