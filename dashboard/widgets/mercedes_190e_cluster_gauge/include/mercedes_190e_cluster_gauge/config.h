#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>
#include <string>
#include "reflection/reflection.h"

REFLECT_STRUCT(sub_gauge_config_t,
    (float, min_value, 0.0),
    (float, max_value, 100.0),
    (std::string, zenoh_key, ""),
    (std::string, schema_type, ""),
    (std::string, value_expression, "")
)

REFLECT_STRUCT(Mercedes190EClusterGaugeConfig_t,
    (sub_gauge_config_t, fuel_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, right_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, bottom_gauge, sub_gauge_config_t{}),
    (sub_gauge_config_t, left_gauge, sub_gauge_config_t{})
)

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H 