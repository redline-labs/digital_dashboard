#ifndef MOTEC_C125_TACHOMETER_CONFIG_H
#define MOTEC_C125_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_STRUCT(MotecC125TachometerConfig_t,
    (uint32_t, max_rpm, 6000),
    (uint32_t, redline_rpm, 5000),
    // uint16_t, not uint8_t: yaml-cpp encodes an 8-bit integer as a character,
    // so this saved as the control byte 0x05 rather than the number 5.
    (uint16_t, center_page_digit, 5),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm),
    (std::string, rpm_expression, "")
)

// max_rpm divides the needle position and bounds both the tick loop and the
// label loop; zero divided, and a value near UINT32_MAX made `rpm += 100` wrap
// so neither loop terminated. redline_rpm above max_rpm draws a red zone that
// runs backwards off the dial.
inline std::vector<std::string> validate(MotecC125TachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    dashboard::limits::clampInto<uint32_t>(cfg.redline_rpm, 0u, cfg.max_rpm, "redline_rpm", notes);
    return notes;
}

#endif // MOTEC_C125_TACHOMETER_CONFIG_H



