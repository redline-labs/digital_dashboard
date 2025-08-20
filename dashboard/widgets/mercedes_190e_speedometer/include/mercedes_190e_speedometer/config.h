#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "reflection/reflection.h"

// Widget-specific configuration structs
REFLECT_STRUCT(Mercedes190ESpeedometerConfig_t,
    (uint32_t, odometer_value, 0),
    (uint16_t, max_speed, 125),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, speed_expression, ""),
    (std::string, odometer_expression, ""),
    (std::string, odometer_zenoh_key, ""),
    (std::string, odometer_schema_type, ""),
    (std::vector<uint8_t>, shift_box_markers, {})
)

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H