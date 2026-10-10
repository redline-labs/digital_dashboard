#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "pub_sub/subscription.h"
#include "pub_sub/topic_key.h"
#include "config_codec/config_limits.h"
#include "dashboard/widget_limits.h"

REFLECT_STRUCT(Mercedes190ETachometerConfig_t,
    (uint16_t, max_rpm, 7000,
        "Maximum RPM", "Full-scale reading at the end of the dial"),
    (uint16_t, redline_rpm, 6000,
        "Redline RPM", "Where the red zone begins; clamped to at most the maximum"),
    (bool, show_clock, true,
        "Show Clock", "Draw the analogue clock inset in the dial face"),
    (pub_sub::subscription_t, rpm, pub_sub::subscriptionFor(pub_sub::schema_type_t::EngineRpm),
        "RPM", "Engine speed: topic, schema, expression and loss-of-comm timeout")
)

// max_rpm scales the dial; redline_rpm above it makes drawRedZone compute a
// negative span and sweep the red arc backwards off the face.
inline std::vector<std::string> validate(Mercedes190ETachometerConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::clampFullScale(cfg.max_rpm, "max_rpm", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.redline_rpm, 0u, cfg.max_rpm, "redline_rpm", notes);
    return notes;
}

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H