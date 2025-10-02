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
    (Mercedes190ETelltaleType, telltale_type, Mercedes190ETelltaleType::battery),
    (helpers::Color, warning_color, "#FF0000"),
    (helpers::Color, normal_color, "#333333"),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, condition_expression, "")
)

#endif // MERCEDES_190E_TELLTALES_CONFIG_H