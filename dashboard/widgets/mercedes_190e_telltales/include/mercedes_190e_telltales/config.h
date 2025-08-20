#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>
#include "reflection/reflection.h"


REFLECT_ENUM(Mercedes190ETelltaleType,
    battery,
    brake_system,
    high_beam,
    windshield_washer
)

REFLECT_STRUCT(Mercedes190ETelltaleConfig_t,
    (Mercedes190ETelltaleType, telltale_type, Mercedes190ETelltaleType::battery),
    (std::string, warning_color, "#FF0000"),
    (std::string, normal_color, "#333333"),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, condition_expression, "")
)

#endif // MERCEDES_190E_TELLTALES_CONFIG_H