#ifndef MOTEC_CDL3_TACHOMETER_CONFIG_H
#define MOTEC_CDL3_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>

struct MotecCdl3TachometerConfig_t {
    MotecCdl3TachometerConfig_t()
        : max_rpm{8000},
          zenoh_key{},
          schema_type{"EngineRpm"},
          rpm_expression{"rpm"}
    {}

    uint32_t max_rpm;           // Full-scale RPM

    // Optional live data hookup
    std::string zenoh_key;      // Topic to subscribe to
    std::string schema_type;    // Cap'n Proto schema type name
    std::string rpm_expression; // Expression to extract rpm
};

#endif // MOTEC_CDL3_TACHOMETER_CONFIG_H


