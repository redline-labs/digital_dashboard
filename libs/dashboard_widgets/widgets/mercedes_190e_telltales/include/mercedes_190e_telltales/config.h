#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"

#include "config_codec/config_limits.h"
#include "helpers/color.h"
#include "reflection/reflection.h"
#include "pub_sub/subscription.h"
#include "pub_sub/topic_key.h"

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
    (pub_sub::subscription_t, condition, pub_sub::subscriptionFor(pub_sub::schema_type_t::VehicleSpeed),
        "Condition", "Lit while this is non-zero: topic, schema, expression and loss-of-comm timeout")
)

#endif // MERCEDES_190E_TELLTALES_CONFIG_H