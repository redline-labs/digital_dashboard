#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>
#include "expression_parser/schema_registry.h"
#include "reflection/reflection.h"

REFLECT_STRUCT(Mercedes190ETachometerConfig_t,
    (uint16_t, max_rpm, 7000),
    (uint16_t, redline_rpm, 6000),
    (bool, show_clock, true),
    (std::string, zenoh_key, ""),
    (schema_type_t, schema_type, schema_type_t::EngineRpm),
    (std::string, rpm_expression, "")
)

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H