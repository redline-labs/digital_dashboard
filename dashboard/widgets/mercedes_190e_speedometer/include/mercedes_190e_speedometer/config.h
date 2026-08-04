#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

// Widget-specific configuration structs
REFLECT_STRUCT(Mercedes190ESpeedometerConfig_t,
    (uint32_t, odometer_value, 0),
    (uint16_t, max_speed, 125),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, speed_expression, ""),
    (std::string, odometer_expression, ""),
    (std::string, odometer_zenoh_key, ""),
    (pub_sub::schema_type_t, odometer_schema_type, pub_sub::schema_type_t::VehicleOdometer),
    // uint16_t, not uint8_t: yaml-cpp treats an 8-bit integer as a character,
    // so `[28, 54, 87]` was emitted as `["\x1c", 6, W]` -- a raw control byte in
    // the file and a different list on the way back in. Nothing here needs to be
    // one byte wide, and a wider type keeps the YAML numeric.
    (std::vector<uint16_t>, shift_box_markers, {})
)

// max_speed scales the dial and divides the needle position. The odometer
// renders six digits, so a larger value silently displayed the wrong ones -- the
// zenoh setter clamped, but the value straight from the config did not. And the
// marker list is walked on every paint.
inline std::vector<std::string> validate(Mercedes190ESpeedometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampInto<uint16_t>(cfg.max_speed, 1u, 1000u, "max_speed", notes);
    dashboard::limits::clampInto<uint32_t>(cfg.odometer_value, 0u, 999999u, "odometer_value", notes);
    dashboard::limits::capLength(cfg.shift_box_markers, dashboard::limits::kMaxMarkers,
                                 "shift_box_markers", notes);
    return notes;
}

#endif // MERCEDES_190E_SPEEDOMETER_CONFIG_H