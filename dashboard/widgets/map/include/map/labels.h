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

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "mvt/tile.h"

#include "map/label_candidates.h"

#include "map/projection.h"
#include "map/style.h"

class QPainter;

namespace map_widget
{

// A tile that may carry names. Same shape as the GPU batch, but holding the
// extracted candidates rather than triangles.
struct LabelTile
{
    TileId id;
    std::shared_ptr<const LabelSet> labels;
};

// Which place classes get a label, and how important each is. A city keeps its
// name and a hamlet loses it, rather than whichever tile decoded first winning.
//
// Only the ORDER of the returned values means anything. They are spaced so that
// a layer can rank exactly half way between two classes; see labels.cpp.
int placePriority(std::string_view className);

// How a single label candidate ranks against the others on screen.
//
// Two keys rather than one because the questions are different. `tier` is what
// KIND of thing this is -- a country beats a city beats a hamlet, whatever
// their sizes. `magnitude` settles a collision between two of the same kind,
// and without it that is decided by which tile happened to decode first.
struct LabelRank
{
    int tier { 0 };
    std::uint32_t magnitude { 0 };
};

// The rank of one feature of the basemap's `place` layer.
//
// Prefers the `rank` and `population` attributes map_build writes over the
// class name: the class alone cannot tell a town of 400 000 from one of 400,
// and a large town should outrank a small city rather than lose to it on a tag
// whose meaning shifts between countries. Falls back to `placePriority()` for
// an archive built before `rank` was written.
LabelRank placeRank(const mvt::Layer& layer, const mvt::Feature& feature);

// The rank of one feature of the tracks overlay's `track_label` layer.
//
// Sits between a town and a city, and orders circuits among themselves by the
// length-derived rank map_build writes, so that where two collide the bigger
// circuit keeps its name.
LabelRank trackRank(const mvt::Layer& layer, const mvt::Feature& feature);

// The rank of one feature of the basemap's `transportation_name` layer.
//
// Sits between a neighbourhood and a locality: a street name must not push a
// town off the map, but at the zooms road labels appear at it is worth more
// than the name of a junction three miles away. Ordered among roads by
// roadPriority(), so the bigger road keeps its name.
LabelRank roadRank(const mvt::Layer& layer, const mvt::Feature& feature);

// The rank of one feature of the basemap's `water_name` layer.
//
// Below a road name. On a map that exists to be driven, the street you are on
// outranks the river you are crossing -- and a river is a long feature that
// would otherwise repeat its way across the viewport at a road's expense.
// Ordered among themselves so a river beats a stream and a lake beats a pond.
LabelRank waterRank(const mvt::Layer& layer, const mvt::Feature& feature);

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

    // One character, rendered twice: the halo and the fill, as separate
    // images.
    //
    // TWO images and not one, which is the whole trap in drawing curved text
    // this way. A road label is a row of individually rotated glyphs, blitted
    // one after another. If each were a finished halo-plus-text sprite, the
    // next glyph's opaque halo would paint over the previous glyph's TEXT
    // wherever the two overlap -- which is everywhere they kern tightly -- and
    // the word comes out gnawed. So every halo is laid down first, then every
    // fill on top.
    //
    // Point labels do not need this and do not pay for it: they are horizontal
    // and keep the whole-string entry above, which is one blit and stays
    // pixel-snapped.
    struct Glyph
    {
        QImage halo;
        QImage fill;
        // Where the pen sits -- the glyph's origin on its baseline -- inside
        // those two images, measured from their top left.
        QPointF pen;
        // How far the pen moves for this character. Layout advances only, with
        // no kerning: a curved label positions each glyph on its own piece of
        // road, so there is no pair for a kerning table to be about.
        double advance { 0.0 };
    };

    // The glyph tier. Keyed on the SAME font/halo/colour/ratio key as the
    // string tier and cleared with it, so a style change cannot leave half the
    // alphabet in the old colours.
    //
    // Far more reusable than the string tier it sits beside: a drive through a
    // thousand street names still only ever touches an alphabet.
    const Glyph& glyphFor(QChar ch, const QFont& font, double haloWidth,
                          const QColor& haloColour, const QColor& textColour,
                          double devicePixelRatio);

    // How far the pen moves for one character, WITHOUT rendering it.
    //
    // The same split as measure() against entryFor(), and load-bearing for the
    // same reason. Laying a name along a road needs an advance per character
    // for every candidate on screen -- hundreds of them -- while only the two
    // dozen that survive collision ever need pixels. Rendering the alphabet in
    // order to find out how wide a name is measured 4.7 ms a frame against
    // 0.6 ms; this is the difference.
    double advanceFor(QChar ch, const QFont& font);

    // `font`, the halo width and the two colours are all baked into the image,
    // so they form the cache key; a change to any of them empties it.
    //
    // The returned reference is valid until the NEXT call. An insertion may
    // rehash and an eviction may remove, and either can move what is already
    // held -- so use the entry and let go of it, which is what the paint loop
    // does.
    const Entry& entryFor(const QString& text, const QFont& font, double haloWidth,
                          const QColor& haloColour, const QColor& textColour,
                          double devicePixelRatio);

    std::size_t size() const { return static_cast<std::size_t>(mEntries.size()); }
    // The text's bounding box, WITHOUT rendering it.
    //
    // Placement needs a size for every candidate, but only the handful that
    // survive collision need pixels. Rendering a label costs 0.88 ms; measuring
    // one is a font-metrics lookup.
    //
    // Measured on a real archive the whole label pass is ~2.8 ms a frame, so
    // this is not load-bearing for performance today; it is here because
    // asking for pixels in order to find out a width is wrong on its own
    // terms, and it keeps the cost flat as the candidate list grows.
    const QRectF& measure(const QString& text, const QFont& font);

    // Whether a rendered label for `text` is in hand. Exposed so that eviction
    // policy is testable: "still cached after 600 other names went past" is the
    // only observable difference between dropping the oldest and dropping the
    // one the viewport is still showing.
    bool contains(const QString& text) const { return mEntries.contains(text); }
    void clear()
    {
        mEntries.clear();
        mOrder.clear();
        mMeasured.clear();
        mGlyphs.clear();
    }

    // How many characters the glyph tier is holding. Exposed for the same
    // reason size() is: it is the only way a test can tell "rendered once and
    // reused" from "rendered every frame".
    std::size_t glyphCount() const { return static_cast<std::size_t>(mGlyphs.size()); }

  private:
    // Move `text` to the most-recently-used end. Called on every hit, so that
    // eviction drops a label that has left the screen rather than one the
    // viewport is still showing.
    void touch(const QString& text);

    // Everything baked into a rendered image. Compared by value, and
    // deliberately NOT via QFont::key(): that builds and formats a QString,
    // and this is checked once per CHARACTER of every candidate on screen --
    // thousands of times a frame. Formatting a key to discover that nothing
    // changed cost 4 ms a frame, which was the whole label budget.
    struct StyleKey
    {
        QFont font;
        double haloWidth { 0.0 };
        QRgb haloColour { 0 };
        QRgb textColour { 0 };
        double devicePixelRatio { 1.0 };

        friend bool operator==(const StyleKey&, const StyleKey&) = default;
    };

    // Re-key the image tiers together, emptying them if anything changed.
    void rekey(const StyleKey& key);
    // The metric tiers depend on the font alone: a colour change does not move
    // a glyph, and re-measuring the alphabet because the halo got wider is
    // work for nothing.
    void refont(const QFont& font);

    StyleKey mKey;
    QHash<QString, Entry> mEntries;
    QHash<char32_t, Glyph> mGlyphs;
    // Measurements, keyed by text within the current font. Far cheaper to hold
    // than rendered images, and needed for every candidate rather than every
    // placed label.
    QFont mMeasureFont;
    bool mHaveMeasureFont { false };
    QHash<QString, QRectF> mMeasured;
    // Advances, within the same font key as mMeasured and cleared with it.
    QHash<char32_t, double> mAdvances;
    // Least recently used first. Parallel to mEntries and holding the same
    // keys; the two are only ever changed together.
    QList<QString> mOrder;
};

LabelStats paintLabels(QPainter& painter, const Projection& projection,
                       const std::vector<LabelTile>& tiles, const MapStyle_t& style,
                       LabelCache& cache);

} // namespace map_widget

#endif // MAP_LABELS_H
