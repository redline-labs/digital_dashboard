// SPDX-License-Identifier: GPL-3.0-or-later
//
// Place names, drawn with QPainter over the GPU frame.
//
// The one part of the map that stays on the CPU, for two reasons that are not
// about speed:
//
//   * Labels must NOT rotate with the map. A rotating map with rotating text is
//     unreadable at every bearing but north, so they cannot ride along in the
//     same transform as the geometry.
//
//   * Collision is viewport-global. Which labels survive depends on what else
//     is already on screen, across tiles -- so they cannot be baked per tile,
//     which is what the GPU path caches.
//
// QPainter also brings the font stack for free, which is the whole reason this
// renderer needs no glyph atlas, no sprite sheet and no font pipeline.
#ifndef MAP_LABELS_H
#define MAP_LABELS_H

#include <memory>
#include <string_view>
#include <vector>

#include "mvt/tile.h"

#include "map/projection.h"
#include "map/style.h"

class QPainter;

namespace map_widget
{

// A tile whose `place` layer may carry names. Same shape as the GPU batch, but
// holding the decoded tile rather than its triangles.
struct LabelTile
{
    TileId id;
    std::shared_ptr<const mvt::Tile> tile;
};

// Which place classes get a label, and how important each is. A city keeps its
// name and a hamlet loses it, rather than whichever tile decoded first winning.
int placePriority(std::string_view className);

struct LabelStats
{
    int placed { 0 };
    int suppressed { 0 };
};

LabelStats paintLabels(QPainter& painter, const Projection& projection,
                       const std::vector<LabelTile>& tiles, const MapStyle_t& style);

} // namespace map_widget

#endif // MAP_LABELS_H
