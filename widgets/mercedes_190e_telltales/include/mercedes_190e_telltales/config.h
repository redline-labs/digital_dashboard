#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>


struct Mercedes190EBatteryTelltaleConfig_t {
    Mercedes190EBatteryTelltaleConfig_t() :
        warning_color{"#FF0000"},
        normal_color{"#333333"},
        zenoh_key{},
        condition_expression{"batteryVoltage < 12.0"}
    {}

    std::string warning_color;          // Color when warning is active
    std::string normal_color;           // Color when in normal state
    std::string zenoh_key;              // Optional Zenoh subscription key (legacy)
    std::string condition_expression;   // Expression to evaluate for telltale state
};

#endif // MERCEDES_190E_TELLTALES_CONFIG_H