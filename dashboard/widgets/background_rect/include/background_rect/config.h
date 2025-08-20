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

#endif // BACKGROUND_RECT_CONFIG_H
