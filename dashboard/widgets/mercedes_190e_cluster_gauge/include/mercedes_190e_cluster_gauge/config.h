#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>
#include <string>

// Widget-specific configuration structs
struct Mercedes190EClusterGaugeConfig_t {
    struct sub_gauge_config_t {
        sub_gauge_config_t() :
            min_value{0.0f},
            max_value{100.0f},
            zenoh_key{},
            schema_type{},
            value_expression{}
        {}
        
        float min_value;           // Minimum value for this sub-gauge
        float max_value;           // Maximum value for this sub-gauge
        std::string zenoh_key;     // Zenoh subscription key for this sub-gauge
        std::string schema_type;   // Cap'n Proto schema type for this sub-gauge
        std::string value_expression; // Expression to evaluate for this sub-gauge value
    };

    Mercedes190EClusterGaugeConfig_t() :
        fuel_gauge{},
        right_gauge{},
        bottom_gauge{},
        left_gauge{}
    {
        // Top gauge (12 o'clock) - Fuel level
        fuel_gauge.min_value = 0.0f;
        fuel_gauge.max_value = 100.0f;
        fuel_gauge.zenoh_key = "";
        fuel_gauge.schema_type = "";
        fuel_gauge.value_expression = "";
        
        // Right gauge (3 o'clock) - Oil pressure
        right_gauge.min_value = 0.0f;
        right_gauge.max_value = 3.0f;
        right_gauge.zenoh_key = "";
        right_gauge.schema_type = "";
        right_gauge.value_expression = "";
        
        // Bottom gauge (6 o'clock) - Battery voltage
        bottom_gauge.min_value = 8.0f;
        bottom_gauge.max_value = 16.0f;
        bottom_gauge.zenoh_key = "";
        bottom_gauge.schema_type = "";
        bottom_gauge.value_expression = "";
        
        // Left gauge (9 o'clock) - Coolant temperature
        left_gauge.min_value = 40.0f;
        left_gauge.max_value = 120.0f;
        left_gauge.zenoh_key = "";
        left_gauge.schema_type = "";
        left_gauge.value_expression = "";
    }

    sub_gauge_config_t fuel_gauge;    // 12 o'clock position
    sub_gauge_config_t right_gauge;  // 3 o'clock position
    sub_gauge_config_t bottom_gauge; // 6 o'clock position
    sub_gauge_config_t left_gauge;   // 9 o'clock position
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H 