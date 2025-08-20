#ifndef BACKGROUND_RECT_CONFIG_H
#define BACKGROUND_RECT_CONFIG_H

#include <string>
#include <vector>

enum class GradientDirection { Vertical, Horizontal };

struct BackgroundRectConfig_t {
	BackgroundRectConfig_t() :
		colors{"#000000"},
		direction{GradientDirection::Horizontal}
	{}

	// One or more color strings in #RRGGBB; if 1 color → uniform
	std::vector<std::string> colors;
	GradientDirection direction; // Vertical or Horizontal
};

#endif // BACKGROUND_RECT_CONFIG_H
