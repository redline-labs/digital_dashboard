#ifndef MOTEC_C125_TACHOMETER_CONFIG_H
#define MOTEC_C125_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "helpers/color.h"
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
    (std::string, rpm_expression, ""),

    // The banner above the gear digit. On the real display this names the
    // active page -- PRACTICE, WARM-UP, RACE -- which is why it is text rather
    // than a fixed label.
    (std::string, page_label, "RACE"),
    // Printed under the gear digit.
    (std::string, scale_label, "RPMx1000"),
    // The MoTeC screens are set in an italic face throughout.
    (bool, italic, true),

    (helpers::Color, fill_color, "#FFB400"),
    (helpers::Color, redline_color, "#DC0000"),
    (helpers::Color, ring_color, "#C8C8C8"),
    (helpers::Color, digit_color, "#FFFFFF")
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



