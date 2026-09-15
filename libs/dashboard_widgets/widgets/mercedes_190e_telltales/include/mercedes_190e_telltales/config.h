#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "reflection/reflection.h"

#include <vector>

REFLECT_ENUM(Mercedes190ETelltaleType,
    battery,
    brake_system,
    high_beam,
    windshield_washer
)

REFLECT_STRUCT(Mercedes190ETelltaleConfig_t,
    (Mercedes190ETelltaleType, telltale_type, Mercedes190ETelltaleType::battery,
        "Telltale", "Which warning symbol this lamp draws"),
    (helpers::Color, warning_color, "#FF0000",
        "Warning Color", "Colour of the lamp while the condition holds"),
    (helpers::Color, normal_color, "#333333",
        "Normal Color", "Colour of the lamp the rest of the time"),
    (std::string, zenoh_key, "",
        "Zenoh Key", "Zenoh topic key to subscribe to"),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed,
        "Schema Type", "Data schema type for the subscription"),
    (std::string, condition_expression, "",
        "Condition Expression", "Expression evaluated against the message; the lamp lights when it is non-zero"),
    // How long a gap in the stream means "no data". 0 never reports one.
    (uint32_t, stale_after_ms, 0,
        "Stale After (ms)", "Show the no-data look when nothing arrives for this long; 0 = never")
)

inline std::vector<std::string> validate(Mercedes190ETelltaleConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampStaleAfter(cfg.stale_after_ms, "stale_after_ms", notes);
    return notes;
}

#endif // MERCEDES_190E_TELLTALES_CONFIG_H