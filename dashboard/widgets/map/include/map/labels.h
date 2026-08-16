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

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>

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

// Rendered labels, kept between frames.
//
// A label is a glyph outline stroked for the halo and then filled. MEASURED at
// 0.88 ms for ONE label at 12 pt -- antialiased path stroking with round joins
// is that expensive in the raster engine, and it dwarfed everything else in
// the paint including the entire GPU frame. Collecting the candidates, by
// contrast, was 0.001 ms.
//
// Nothing about a label's appearance changes between frames; only where it
// goes. So each one is rendered once into a small transparent image and blitted
// after that.
//
// Owned by the caller because it must outlive a single paint. GUI thread only,
// like everything else in the label pass.
class LabelCache
{
  public:
    struct Entry
    {
        // Text and halo, on transparency, padded so the halo is not clipped.
        QImage image;
        // The UNPADDED text rect. Placement and collision use this, so adding
        // padding for the halo does not make labels claim more space than they
        // did.
        QRectF bounds;
        // Where to blit, relative to the placement box's top left.
        QPointF offset;
    };

    // `font`, the halo width and the two colours are all baked into the image,
    // so they form the cache key; a change to any of them empties it.
    const Entry& entryFor(const QString& text, const QFont& font, double haloWidth,
                          const QColor& haloColour, const QColor& textColour,
                          double devicePixelRatio);

    std::size_t size() const { return static_cast<std::size_t>(mEntries.size()); }
    void clear() { mEntries.clear(); }

  private:
    QString mKey;
    QHash<QString, Entry> mEntries;
};

LabelStats paintLabels(QPainter& painter, const Projection& projection,
                       const std::vector<LabelTile>& tiles, const MapStyle_t& style,
                       LabelCache& cache);

} // namespace map_widget

#endif // MAP_LABELS_H
