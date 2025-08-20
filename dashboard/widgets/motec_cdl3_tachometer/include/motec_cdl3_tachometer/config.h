#ifndef MOTEC_CDL3_TACHOMETER_CONFIG_H
#define MOTEC_CDL3_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "reflection/reflection.h"

REFLECT_STRUCT(MotecCdl3TachometerConfig_t,
    (uint32_t, max_rpm, 6000),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, rpm_expression, "")
)

#endif // MOTEC_CDL3_TACHOMETER_CONFIG_H


