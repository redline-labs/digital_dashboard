#ifndef BACKGROUND_RECT_CONFIG_H
#define BACKGROUND_RECT_CONFIG_H

#include <string>
#include <vector>

#include "helpers/color.h"
#include "reflection/reflection.h"

REFLECT_ENUM(GradientDirection,
    vertical,
    horizontal
)

REFLECT_STRUCT(BackgroundRectConfig_t,
    (std::vector<helpers::Color>, colors, {},
        "Gradient Colors", "List of colors for the gradient (hex format). Single color for solid fill."),
    (GradientDirection, direction, GradientDirection::vertical,
        "Gradient Direction", "Direction of the gradient (vertical or horizontal)")
)

#endif // BACKGROUND_RECT_CONFIG_H
