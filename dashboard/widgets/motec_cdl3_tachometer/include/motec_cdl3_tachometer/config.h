#ifndef MOTEC_CDL3_TACHOMETER_CONFIG_H
#define MOTEC_CDL3_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_STRUCT(MotecCdl3TachometerConfig_t,
    (uint32_t, max_rpm, 6000),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm),
    (std::string, rpm_expression, "")
)

REFLECT_METADATA(MotecCdl3TachometerConfig_t,
    (max_rpm, "Maximum RPM", "Full-scale reading; sets how many segments the bar spans"),
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (rpm_expression, "RPM Expression", "Expression evaluated against the message to produce engine RPM")
)

// max_rpm is the divisor for the segment count and the bound on the tick loop
// that runs in the constructor. Zero divided; a value near UINT32_MAX made that
// loop allocate until the process died.
inline std::vector<std::string> validate(MotecCdl3TachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    return notes;
}

#endif // MOTEC_CDL3_TACHOMETER_CONFIG_H


