#ifndef MERCEDES_190E_SPEEDOMETER_CONFIG_H
#define MERCEDES_190E_SPEEDOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "config_codec/config_limits.h"

// Widget-specific configuration structs
REFLECT_STRUCT(Mercedes190ESpeedometerConfig_t,
    (uint32_t, odometer_value, 0,
        "Odometer Reading", "Starting reading for the six-digit odometer"),
    (uint16_t, max_speed, 125,
        "Maximum Speed", "Full-scale reading at the end of the dial"),
    (std::string, zenoh_key, "",
        "Speed Zenoh Key", "Zenoh topic key the road speed is read from"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed,
        "Speed Schema Type", "Data schema type for the speed subscription"),
    (std::string, speed_expression, "",
        "Speed Expression", "Expression evaluated against the speed message, in the dial's own units"),
    (std::string, odometer_expression, "",
        "Odometer Expression", "Expression evaluated against the odometer message"),
    (std::string, odometer_zenoh_key, "",
        "Odometer Zenoh Key", "Zenoh topic key the odometer reading is read from"),
    (pub_sub::schema_type_t, odometer_schema_type, pub_sub::schema_type_t::VehicleOdometer,
        "Odometer Schema Type", "Data schema type for the odometer subscription"),
    // uint16_t, not uint8_t: yaml-cpp treats an 8-bit integer as a character,
    // so `[28, 54, 87]` was emitted as `["\x1c", 6, W]` -- a raw control byte in
    // the file and a different list on the way back in. Nothing here needs to be
    // one byte wide, and a wider type keeps the YAML numeric.
    (std::vector<uint16_t>, shift_box_markers, {},
        "Shift Markers", "Speeds, in dial units, at which to draw a shift box on the face")
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