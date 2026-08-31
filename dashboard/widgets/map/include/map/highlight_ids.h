// SPDX-License-Identifier: GPL-3.0-or-later
//
// From a horizon to the way ids worth lighting up. Split from the widget so
// the join is testable without a zenoh session or a paint device.
#ifndef MAP_HIGHLIGHT_IDS_H
#define MAP_HIGHLIGHT_IDS_H

#include <cstdint>
#include <vector>

#include "map_horizon.capnp.h"

namespace map_widget
{

// The matched road and the road ahead of it: the segment under the vehicle,
// plus every SEGMENT profile on the ROOT path (the one the vehicle is on --
// side branches would light up every road at a junction). Profiles are
// filtered, not switched, per the schema's own rule, and segment ids collapse
// to way ids because that is the identity map_build stamps on tile features.
//
// Sorted and deduplicated, which is the renderer's contract -- see
// MapPass::Highlight. Empty when the matcher has no position.
std::vector<std::uint64_t> highlightWayIds(::MapHorizon::Reader horizon);

} // namespace map_widget

#endif // MAP_HIGHLIGHT_IDS_H
