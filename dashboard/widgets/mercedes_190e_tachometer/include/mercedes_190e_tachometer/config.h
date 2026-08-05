#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_STRUCT(Mercedes190ETachometerConfig_t,
    (uint16_t, max_rpm, 7000),
    (uint16_t, redline_rpm, 6000),
    (bool, show_clock, true),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::EngineRpm),
    (std::string, rpm_expression, "")
)

REFLECT_METADATA(Mercedes190ETachometerConfig_t,
    (max_rpm, "Maximum RPM", "Full-scale reading at the end of the dial"),
    (redline_rpm, "Redline RPM", "Where the red zone begins; clamped to at most the maximum"),
    (show_clock, "Show Clock", "Draw the analogue clock inset in the dial face"),
    (zenoh_key, "Zenoh Key", "Zenoh topic key to subscribe to"),
    (schema_type, "Schema Type", "Data schema type for the subscription"),
    (rpm_expression, "RPM Expression", "Expression evaluated against the message to produce engine RPM")
)

// max_rpm scales the dial; redline_rpm above it makes drawRedZone compute a
// negative span and sweep the red arc backwards off the face.
inline std::vector<std::string> validate(Mercedes190ETachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    dashboard::limits::clampInto<uint16_t>(cfg.redline_rpm, 0u, cfg.max_rpm, "redline_rpm", notes);
    return notes;
}

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H