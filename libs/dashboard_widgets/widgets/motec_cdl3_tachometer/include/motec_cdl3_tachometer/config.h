#ifndef MOTEC_CDL3_TACHOMETER_CONFIG_H
#define MOTEC_CDL3_TACHOMETER_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "config_codec/config_limits.h"

REFLECT_STRUCT(MotecCdl3TachometerConfig_t,
    (uint32_t, max_rpm, 6000,
        "Maximum RPM", "Full-scale reading; sets how many segments the bar spans"),
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm,
        "Schema Type", "Data schema type for the subscription"),
    (std::string, rpm_expression, "",
        "RPM Expression", "Expression evaluated against the message to produce engine RPM"),
    // How long a gap in the stream means "no data". 0 never reports one.
    (uint32_t, stale_after_ms, 0,
        "Stale After (ms)", "Show the no-data look when nothing arrives for this long; 0 = never")
)

// max_rpm is the divisor for the segment count and the bound on the tick loop
// that runs in the constructor. Zero divided; a value near UINT32_MAX made that
// loop allocate until the process died.
inline std::vector<std::string> validate(MotecCdl3TachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    config_codec::limits::clampStaleAfter(cfg.stale_after_ms, "stale_after_ms", notes);
    return notes;
}

#endif // MOTEC_CDL3_TACHOMETER_CONFIG_H

