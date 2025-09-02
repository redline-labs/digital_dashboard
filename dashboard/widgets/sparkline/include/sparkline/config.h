#ifndef SPARKLINE_CONFIG_H
#define SPARKLINE_CONFIG_H

#include <string>
#include <cstdint>
#include "expression_parser/schema_registry.h"
#include "reflection/reflection.h"


REFLECT_STRUCT(SparklineConfig_t,
    (std::string, units, "Untitled"),
    (double, min_value, 0.0),
    (double, max_value, 100.0),
    (std::string, line_color, "#0000FF"),
    (std::string, text_color, "#FFFFFF"),
    (std::string, font_family, "Arial"),
    (uint16_t, font_size_value, 24),
    (uint16_t, font_size_units, 10),
    (uint16_t, update_rate, 30),
    (std::string, zenoh_key, ""),
    (schema_type_t, schema_type, schema_type_t::VehicleSpeed),
    (std::string, value_expression, "")
)

#endif // SPARKLINE_CONFIG_H