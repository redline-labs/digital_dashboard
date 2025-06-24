#ifndef SPARKLINE_CONFIG_H
#define SPARKLINE_CONFIG_H

#include <string>
#include <cstdint>


struct sparkline_config_t {
    sparkline_config_t() :
        units{""},
        min_value{0.0},
        max_value{100.0},
        line_color{"#0000FF"},
        update_rate{30}
    {}

    std::string units;          // Display units (e.g., "°C", "mph", "psi")
    double min_value;           // Minimum Y-axis range value
    double max_value;           // Maximum Y-axis range value
    std::string line_color;     // Hex color for line and gradient
    uint16_t update_rate;       // Refresh rate in Hz
};

#endif // SPARKLINE_CONFIG_H