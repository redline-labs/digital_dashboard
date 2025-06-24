#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <string>
#include <cstdint>

// Widget-specific configuration structs
struct speedometer_config_t {
    speedometer_config_t() :
        odometer_value{123456},
        max_speed{160},
        font_family{"futura"}
    {}

    uint32_t odometer_value;    // Starting odometer reading (0-999999)
    uint16_t max_speed;         // Maximum speed on gauge in mph
    std::string font_family;    // Font family for text rendering
};

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H