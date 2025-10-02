#ifndef STATIC_TEXT_CONFIG_H
#define STATIC_TEXT_CONFIG_H

#include <string>
#include <cstdint>
#include "reflection/reflection.h"
#include "helpers/color.h"

REFLECT_STRUCT(StaticTextConfig_t,
    (std::string, text, "Your Text Here"),
    (std::string, font, "Arial"),
    (uint16_t,    font_size, 12),
    (helpers::Color, color, "#000000")
)

REFLECT_METADATA(StaticTextConfig_t,
    (text, "Text Content", "The text to display"),
    (font, "Font Family", "Font family name (e.g., Arial, Times New Roman)"),
    (font_size, "Font Size", "Size of the font in points"),
    (color, "Text Color", "Color in hex format (e.g., #000000 for black)")
)

#endif // STATIC_TEXT_CONFIG_H


