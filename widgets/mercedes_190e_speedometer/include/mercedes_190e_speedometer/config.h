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
        schema_type{""},
        speed_expression{""},  // Convert m/s to mph by default
        odometer_expression{""},                  // Optional odometer expression
        odometer_zenoh_key{},                     // Separate Zenoh key for odometer
        odometer_schema_type{""},  // Separate schema type for odometer
        shift_box_markers{}
    {}

    uint32_t odometer_value;    // Starting odometer reading (0-999999)
    uint16_t max_speed;         // Maximum speed on gauge in mph
    std::string zenoh_key;      // Optional Zenoh subscription key for speed
    std::string schema_type;    // Cap'n Proto schema type for speed (e.g., "VehicleSpeed", "EngineRpm")
    std::string speed_expression;   // Expression to evaluate for speed in mph
    std::string odometer_expression; // Optional expression for odometer updates
    std::string odometer_zenoh_key;   // Separate Zenoh subscription key for odometer
    std::string odometer_schema_type; // Cap'n Proto schema type for odometer (e.g., "VehicleOdometer")
    std::vector<uint8_t> shift_box_markers; // Shift box markers
};

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H