#ifndef MOTEC_C125_TACHOMETER_CONFIG_H
#define MOTEC_C125_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>

struct MotecC125TachometerConfig_t {
    MotecC125TachometerConfig_t() :
        max_rpm{8000},
        warning_rpm{6500},
        center_page_digit{5},
        zenoh_key{},
        schema_type{"EngineRpm"},
        rpm_expression{"rpm"}
    {}

    uint32_t max_rpm;           // Overall maximum for mapping the yellow arc
    uint32_t warning_rpm;       // Start of warning/redline (for future use)
    uint8_t  center_page_digit; // Large digit in the center (static for now)

    // Optional live data hookup (same style as other widgets)
    std::string zenoh_key;      // Topic to subscribe to
    std::string schema_type;    // Cap'n Proto schema type name
    std::string rpm_expression; // Expression to extract rpm
};

#endif // MOTEC_C125_TACHOMETER_CONFIG_H



