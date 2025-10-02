#ifndef BACKGROUND_RECT_CONFIG_H
#define BACKGROUND_RECT_CONFIG_H

#include <string>
#include <vector>

#include "reflection/reflection.h"

REFLECT_ENUM(GradientDirection,
    vertical,
    horizontal
)

REFLECT_STRUCT(BackgroundRectConfig_t,
    (std::vector<std::string>, colors, {}),
    (GradientDirection, direction, GradientDirection::vertical)
)

REFLECT_METADATA(BackgroundRectConfig_t,
    (colors, "Gradient Colors", "List of colors for the gradient (hex format). Single color for solid fill."),
    (direction, "Gradient Direction", "Direction of the gradient (vertical or horizontal)")
)

#endif // BACKGROUND_RECT_CONFIG_H
