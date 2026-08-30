// SPDX-License-Identifier: GPL-3.0-or-later
//
// The camera modes both map surfaces share.
//
// One enum, here rather than in either widget's config, because the dashboard
// map widget and the scope map panel are deliberately separate implementations
// that share exactly the rendering -- and a mode that changes what the
// renderer is asked to draw belongs on the shared side of that line.
// Orientation modes are NOT here: what "heading up" means differs per surface
// (the vehicle's live heading on the dashboard, the track's course under the
// cursor in scope), so each declares its own.
#ifndef MAP_CAMERA_MODES_H
#define MAP_CAMERA_MODES_H

#include "reflection/reflection.h"

// How the camera looks at the ground: straight down, or tilted back to the
// configured pitch. The pitch ANGLE stays a config field beside this; the mode
// is what a button can toggle without forgetting the angle.
REFLECT_ENUM(MapViewMode_t,
    top_down,
    perspective
)

#endif // MAP_CAMERA_MODES_H
