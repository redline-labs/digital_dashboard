#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

// Widget-specific configuration structs
struct Mercedes190ESpeedometerConfig_t {
    Mercedes190ESpeedometerConfig_t() :
        odometer_value{0u},
        max_speed{125u},
        zenoh_key{},
        shift_box_markers{}
    {}

    uint32_t odometer_value;    // Starting odometer reading (0-999999)
    uint16_t max_speed;         // Maximum speed on gauge in mph
    std::string zenoh_key;      // Optional Zenoh subscription key
    std::vector<uint8_t> shift_box_markers; // Shift box markers
};

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H