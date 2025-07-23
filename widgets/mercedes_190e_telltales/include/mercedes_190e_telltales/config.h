#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>


struct Mercedes190EBatteryTelltaleConfig_t {
    Mercedes190EBatteryTelltaleConfig_t() :
        warning_color{"#FF0000"},
        normal_color{"#333333"}
    {}

    std::string warning_color;  // Color when warning is active
    std::string normal_color;   // Color when in normal state
};

#endif // MERCEDES_190E_TELLTALES_CONFIG_H