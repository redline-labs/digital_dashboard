#ifndef STATIC_TEXT_CONFIG_H
#define STATIC_TEXT_CONFIG_H

#include <string>
#include <cstdint>

struct StaticTextConfig_t {
    StaticTextConfig_t()
        : text{""}
        , font{"Arial"}
        , font_size{12}
        , color{"#000000"}
    {}

    std::string text;      // Text content
    std::string font;      // Font family or resource-loaded font family
    uint16_t    font_size; // Point size
    std::string color;     // Hex color #RRGGBB
};

#endif // STATIC_TEXT_CONFIG_H


