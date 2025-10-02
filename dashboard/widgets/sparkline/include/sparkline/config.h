#ifndef SPARKLINE_CONFIG_H
#define SPARKLINE_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "helpers/color.h"


REFLECT_STRUCT(SparklineConfig_t,
    (std::string, units, "Untitled"),
    (double, min_value, 0.0),
    (double, max_value, 100.0),
    (helpers::Color, line_color, "#0000FF"),
    (helpers::Color, text_color, "#FFFFFF"),
    (std::string, font_family, "Arial"),
    (uint16_t, font_size_value, 24),
    (uint16_t, font_size_units, 10),
    (uint16_t, update_rate, 30),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, value_expression, "")
)

REFLECT_METADATA(SparklineConfig_t,
    (units, "Units Label", "Units text to display (e.g., mph, °C)"),
    (min_value, "Minimum Value", "Minimum value for the Y-axis scale"),
    (max_value, "Maximum Value", "Maximum value for the Y-axis scale"),
    (line_color, "Line Color", "Color of the sparkline graph"),
    (text_color, "Text Color", "Color of the value and units text"),
    (font_family, "Font Family", "Font family for the displayed text"),
    (font_size_value, "Value Font Size", "Font size for the numeric value"),
    (font_size_units, "Units Font Size", "Font size for the units label"),
    (update_rate, "Update Rate (Hz)", "Graph update rate in Hertz"),
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (value_expression, "Value Expression", "Expression to extract/compute the value")
)

#endif // SPARKLINE_CONFIG_H