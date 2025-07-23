#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <cstdint>

// Widget-specific configuration structs
struct Mercedes190ESpeedometerConfig_t {
    constexpr Mercedes190ESpeedometerConfig_t() :
        odometer_value{0u},
        max_speed{125u}
    {}

    uint32_t odometer_value;    // Starting odometer reading (0-999999)
    uint16_t max_speed;         // Maximum speed on gauge in mph
};

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H