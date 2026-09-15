#ifndef DASHBOARD_WINDOW_PLACEMENT_H_
#define DASHBOARD_WINDOW_PLACEMENT_H_

#include "reflection/reflection.h"

// Where a window goes, split out of app_config.h so the display binding can be
// reasoned about (and tested) without the widget table that header drags in.

// Which physical display a window belongs on. These are the rootfs's role names:
// fixed aliases for port pairs on the housing, published per boot as
// REDLINE_DISPLAY_<ROLE>_CONNECTOR by redline-display. Off the target nothing
// publishes them and the role is ignored. See docs/apps/dashboard/windows.md.
REFLECT_ENUM(display_role_t, primary, secondary)

// How a window's design size meets the display it is bound to. `fit` scales
// uniformly by the smaller of the two ratios and letterboxes the rest; `none`
// leaves scaling to whatever the environment already says.
REFLECT_ENUM(scale_mode_t, fit, none)

struct WindowPlacement
{
    display_role_t display = display_role_t::primary;
    scale_mode_t scale = scale_mode_t::fit;
    int width = 0;
    int height = 0;
};

#endif  // DASHBOARD_WINDOW_PLACEMENT_H_
