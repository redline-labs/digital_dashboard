#ifndef MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H
#define MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H

#include <cstdint>

// Widget-specific configuration structs
struct Mercedes190EClusterGaugeConfig_t {
    struct sub_gauge_config_t {
        sub_gauge_config_t() :
            min_value{0.0f},
            max_value{100.0f},
            current_value{0.0f}
        {}
        
        float min_value;     // Minimum value for this sub-gauge
        float max_value;     // Maximum value for this sub-gauge  
        float current_value; // Current value for this sub-gauge
    };

    Mercedes190EClusterGaugeConfig_t() :
        top_gauge{},
        right_gauge{},
        bottom_gauge{},
        left_gauge{}
    {
        // Fuel gauge.
        top_gauge.min_value = 0.0f;
        top_gauge.max_value = 100.0f;
        
        // Right gauge (3 o'clock) - Fuel 
        right_gauge.min_value = 0.0f;
        right_gauge.max_value = 100.0f;
        
        // Bottom gauge (6 o'clock) - Oil pressure
        bottom_gauge.min_value = 0.0f;
        bottom_gauge.max_value = 80.0f;
        
        // Left gauge (9 o'clock) - Economy/MPG
        left_gauge.min_value = 0.0f;
        left_gauge.max_value = 40.0f;
    }

    sub_gauge_config_t top_gauge;    // 12 o'clock position
    sub_gauge_config_t right_gauge;  // 3 o'clock position
    sub_gauge_config_t bottom_gauge; // 6 o'clock position
    sub_gauge_config_t left_gauge;   // 9 o'clock position
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_CONFIG_H 