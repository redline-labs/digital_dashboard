#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>
#include "reflection/reflection.h"

REFLECT_STRUCT(Mercedes190ETachometerConfig_t,
    (uint16_t, max_rpm, 7000),
    (uint16_t, redline_rpm, 6000),
    (bool, show_clock, true),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, rpm_expression, "")
)

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H