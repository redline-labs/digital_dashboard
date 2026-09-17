#ifndef PAGE_BUTTON_CONFIG_H
#define PAGE_BUTTON_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"
#include "dashboard/page_command.h"
#include "helpers/color.h"
#include "reflection/reflection.h"

REFLECT_STRUCT(PageButtonConfig_t,
    (std::string, label, "Back",
        "Label", "Text on the button"),
    (page_command_t, command, page_command_t{},
        "Command", "What a tap does: the page_stack to change, and how"),
    (helpers::Color, background_color, "#222222",
        "Background Color", "Fill while not pressed"),
    (helpers::Color, pressed_color, "#444444",
        "Pressed Color", "Fill while a finger is on it"),
    (helpers::Color, text_color, "#FFFFFF",
        "Text Color", "Colour of the label"),
    (uint16_t, font_size, 18,
        "Font Size", "Label size in points"),
    (uint16_t, corner_radius, 8,
        "Corner Radius", "Roundness of the corners, in pixels")
)

inline std::vector<std::string> validate(PageButtonConfig_t& cfg)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto<uint16_t>(cfg.font_size, 4u, 200u, "font_size", notes);
    config_codec::limits::clampInto<uint16_t>(cfg.corner_radius, 0u, 500u, "corner_radius", notes);
    return notes;
}

#endif // PAGE_BUTTON_CONFIG_H
