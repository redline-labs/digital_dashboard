#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>

struct Mercedes190ETachometerConfig_t {
    Mercedes190ETachometerConfig_t() :
        max_rpm{6000},
        redline_rpm{5500},
        show_clock{true},
        zenoh_key{},
        schema_type{"EngineRpm"},
        rpm_expression{"rpm"}      // Direct rpm field by default
    {}

    uint16_t max_rpm;           // Maximum RPM value on gauge
    uint16_t redline_rpm;       // RPM where redline zone begins
    bool show_clock;            // Whether to display digital clock
    std::string zenoh_key;      // Optional Zenoh subscription key
    std::string schema_type;    // Cap'n Proto schema type (e.g., "EngineRpm", "VehicleSpeed")
    std::string rpm_expression; // Expression to evaluate for RPM value
};

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H