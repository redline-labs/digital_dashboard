#ifndef STATIC_TEXT_CONFIG_H
#define STATIC_TEXT_CONFIG_H

#include <string>
#include <cstdint>
#include "reflection/reflection.h"

REFLECT_STRUCT(StaticTextConfig_t,
    (std::string, text, "Your Text Here"),
    (std::string, font, "Arial"),
    (uint16_t,    font_size, 12),
    (std::string, color, "#000000")
)

#endif // STATIC_TEXT_CONFIG_H


