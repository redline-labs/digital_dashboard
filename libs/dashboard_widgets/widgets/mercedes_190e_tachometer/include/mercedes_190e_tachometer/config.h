#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "config_codec/config_limits.h"

REFLECT_STRUCT(Mercedes190ETachometerConfig_t,
    (uint16_t, max_rpm, 7000,
        "Maximum RPM", "Full-scale reading at the end of the dial"),
    (uint16_t, redline_rpm, 6000,
        "Redline RPM", "Where the red zone begins; clamped to at most the maximum"),
    (bool, show_clock, true,
        "Show Clock", "Draw the analogue clock inset in the dial face"),
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm,
        "Schema Type", "Data schema type for the subscription"),
    (std::string, rpm_expression, "",
        "RPM Expression", "Expression evaluated against the message to produce engine RPM")
)

// max_rpm scales the dial; redline_rpm above it makes drawRedZone compute a
// negative span and sweep the red arc backwards off the face.
inline std::vector<std::string> validate(Mercedes190ETachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.redline_rpm, 0u, cfg.max_rpm, "redline_rpm", notes);
    return notes;
}

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H