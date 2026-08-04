#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>
#include <string>
#include "pub_sub/schema_registry.h"
#include "reflection/reflection.h"
#include "dashboard/config_limits.h"

REFLECT_STRUCT(sub_gauge_config_t,
    (float, min_value, 0.0),
    (float, max_value, 100.0),
    (std::string, zenoh_key, ""),
    (pub_sub::schema_type_t, schema_type, pub_sub::schema_type_t::VehicleSpeed),
    (std::string, value_expression, "")
)

REFLECT_STRUCT(Mercedes190EClusterGaugeConfig_t,
    (sub_gauge_config_t, fuel_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, right_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, bottom_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, left_gauge, sub_gauge_config_t{})
)

// Each sub-gauge clamps incoming readings to its own min/max. Those come
// straight from YAML with nothing checking their order, and std::clamp's
// precondition is !(max < min) -- an inverted pair was undefined behaviour, four
// times over.
inline std::vector<std::string> validate(Mercedes190EClusterGaugeConfig_t& cfg)
{
    std::vector<std::string> notes;
    dashboard::limits::orderRange(cfg.fuel_gauge.min_value, cfg.fuel_gauge.max_value,
                                  "fuel_gauge", notes);
    dashboard::limits::orderRange(cfg.right_gauge.min_value, cfg.right_gauge.max_value,
                                  "right_gauge", notes);
    dashboard::limits::orderRange(cfg.bottom_gauge.min_value, cfg.bottom_gauge.max_value,
                                  "bottom_gauge", notes);
    dashboard::limits::orderRange(cfg.left_gauge.min_value, cfg.left_gauge.max_value,
                                  "left_gauge", notes);
    return notes;
}

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H