#ifndef SPARKLINE_CONFIG_H
#define SPARKLINE_CONFIG_H

#include <string>
#include <cstdint>


struct SparklineConfig_t {
    SparklineConfig_t() :
        units{""},
        min_value{0.0},
        max_value{100.0},
        line_color{"#0000FF"},
        text_color{"#FFFFFF"},
        font_family{"Arial"},
        font_size_value{24},
        font_size_units{10},
        update_rate{30},
        zenoh_key{},
        schema_type{""},
        value_expression{""}
    {}

    std::string units;          // Display units (e.g., "°C", "mph", "psi")
    double min_value;           // Minimum Y-axis range value
    double max_value;           // Maximum Y-axis range value
    std::string line_color;     // Hex color for line and gradient
    std::string text_color;     // Hex color for value and units text
    std::string font_family;    // Font family name (e.g., "Arial", "Times", "Courier")
    uint16_t font_size_value;   // Font size for the value text in points
    uint16_t font_size_units;   // Font size for the units text in points
    uint16_t update_rate;       // Refresh rate in Hz
    std::string zenoh_key;      // Optional Zenoh subscription key
    std::string schema_type;    // Cap'n Proto schema type
    std::string value_expression; // Expression to compute the sparkline value
};

#endif // SPARKLINE_CONFIG_H