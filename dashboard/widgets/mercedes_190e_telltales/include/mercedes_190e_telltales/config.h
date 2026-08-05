#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"

#include "helpers/color.h"
#include "reflection/reflection.h"

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
        "Condition Expression", "Expression evaluated against the message; the lamp lights when it is non-zero")
)

#endif // MERCEDES_190E_TELLTALES_CONFIG_H