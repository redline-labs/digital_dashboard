#ifndef MOTEC_CDL3_TACHOMETER_CONFIG_H
#define MOTEC_CDL3_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"

REFLECT_STRUCT(MotecCdl3TachometerConfig_t,
    (uint32_t, max_rpm, 6000),
    (std::string, zenoh_key, ""),
    (schema_type_t, schema_type, schema_type_t::EngineRpm),
    (std::string, rpm_expression, "")
)

#endif // MOTEC_CDL3_TACHOMETER_CONFIG_H


