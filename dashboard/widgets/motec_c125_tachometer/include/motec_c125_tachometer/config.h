#ifndef MOTEC_C125_TACHOMETER_CONFIG_H
#define MOTEC_C125_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "reflection/reflection.h"

REFLECT_STRUCT(MotecC125TachometerConfig_t,
    (uint32_t, max_rpm, 6000),
    (uint32_t, warning_rpm, 5000),
    (uint32_t, redline_rpm, 0),
    (uint8_t,  center_page_digit, 5),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, rpm_expression, "")
)

#endif // MOTEC_C125_TACHOMETER_CONFIG_H



