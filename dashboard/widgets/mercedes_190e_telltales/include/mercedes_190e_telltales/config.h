#ifndef MERCEDES_190E_TELLTALES_CONFIG_H
#define MERCEDES_190E_TELLTALES_CONFIG_H

#include <string>
#include <cstdint>


enum class Mercedes190ETelltaleType
{
    battery,
    brake_system,
    high_beam,
    windshield_washer
};

struct Mercedes190ETelltaleConfig_t
{
    Mercedes190ETelltaleConfig_t() :
        telltale_type{Mercedes190ETelltaleType::battery},
        warning_color{"#FF0000"},
        normal_color{"#333333"},
        zenoh_key{},
        schema_type{""},
        condition_expression{""}
    {
    }

    Mercedes190ETelltaleType telltale_type; // Which icon to render
    std::string warning_color;          // Color when warning is active
    std::string normal_color;           // Color when in normal state
    std::string zenoh_key;              // Zenoh subscription key for data
    std::string schema_type;            // Cap'n Proto schema type (e.g., "BatteryWarning", "EngineRpm")
    std::string condition_expression;   // Expression to evaluate for telltale state
};

#endif // MERCEDES_190E_TELLTALES_CONFIG_H