// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/labels.h"

// For roadPriority(): the ladder that decides which layer a road is drawn in is
// the same one that decides whose name survives a collision.
#include "map/tessellator.h"

#include "qt_helpers/widget_colors.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QSet>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <variant>

namespace map_widget
{
namespace
{

struct Candidate
{
    QString text;
    ScreenPoint at;
    // How much room the feature itself occupies on screen, for a line label:
    // a name wider than the road it names reads as floating text. Zero means
    // "no constraint", which is every point label.
    double spanPx { 0.0 };
    bool oneLabelPerName { false };
    // Coarse tier: which KIND of thing this is. A country outranks a city
    // outranks a hamlet, whatever their sizes.
    int priority { 0 };
    // Tie-break within a tier, higher first: population for a place, track
    // length for a circuit. Two cities competing for the same pixels should
    // not be settled by which tile decoded first.
    std::uint32_t magnitude { 0 };
};

// Which source layers carry labels, and how each ranks its own.
//
// A TABLE rather than a hardcoded layer(), because there is more than one now:
// the basemap's `place` and the track archive's `track_label`, which arrive in
// the same viewport from two different archives. They compete for the same
// screen space and are placed by one pass, which is the whole reason the
// candidates are gathered before any of them is drawn.
// Tiers step by TWO, not one, so that a layer can sit exactly half way between
// two of them. The track layer is the reason: it is documented as ranking
// between a town and a city, and on a unit scale there is no such number.
constexpr int kPlaceTierStep = 2;

// Point labels sit AT a coordinate; line labels sit along one. The difference
// is not cosmetic -- a road's name has to be placed from geometry that may be
// forty miles long and clipped into a dozen tiles, and the same name then turns
// up once per tile it crosses.
enum class LabelGeometry
{
    Point,
    Line,
};

struct LabelLayerSpec
{
    const char* sourceLayer;
    LabelRank (*priority)(const mvt::Layer&, const mvt::Feature&);
    LabelGeometry geometry { LabelGeometry::Point };
    // Whether this layer draws at all, at this camera zoom. Null means always
    // -- which is right for places, whose own archive minzoom is the only
    // sensible floor.
    bool (*enabled)(const MapStyle_t&, double zoom) { nullptr };
    // Place at most one label per distinct name. Only lines need it: a place
    // name appears once, in one tile, and duplicates from a stand-in ancestor
    // land on the same pixels and lose the collision test. A road crosses every
    // tile it passes through and each one carries the whole name, so without
    // this a single street is labelled a dozen times across the viewport.
    bool oneLabelPerName { false };
};

bool roadLabelsEnabled(const MapStyle_t& style, double zoom)
{
    return style.show_road_labels && zoom >= double(style.detail.road_label);
}

constexpr std::array<LabelLayerSpec, 3> kLabelLayers { {
    { "place", placeRank },
    { "track_label", trackRank },
    { "transportation_name", roadRank, LabelGeometry::Line, roadLabelsEnabled,
      /*oneLabelPerName=*/true },
} };

// Every labelled point in one source layer of one tile, as screen-space
// candidates.
//
// Gathered rather than placed, because a label's position depends on which
// labels were already accepted and the order tiles arrive in is decode order --
// which is not an order anybody chose. With two source layers it matters more
// than it did with one: a circuit's name and a town's name land in the same
// place from two different archives, and only one of them can have it.
void gatherLabels(const LabelTile& entry, const LabelLayerSpec& spec,
                  const Projection& projection, const QRectF& viewport,
                  std::vector<Candidate>& candidates)
{
    const mvt::Layer* layer = entry.tile->layer(spec.sourceLayer);
    if (layer == nullptr || layer->extent == 0)
    {
        return;
    }

    const ScreenPoint origin = projection.tileOrigin(entry.id);
    const double size = projection.tileScreenSize(entry.id.z);
    const double scale = size / double(layer->extent);
    // The projection's own rotation terms, not a local re-derivation: three
    // copies of "which way does the map turn" is two too many.
    const double cosB = projection.bearingCos();
    const double sinB = projection.bearingSin();

    // Tile-local to screen. The tile's own axes are rotated with the map even
    // though the text is not, so an anchor has to go through the same rotation
    // the GPU applies to the geometry.
    const auto toScreen = [&](const mvt::Point& p) {
        const double lx = double(p.x) * scale;
        const double ly = double(p.y) * scale;
        return ScreenPoint { origin.x + ((lx * cosB) - (ly * sinB)),
                             origin.y + ((lx * sinB) + (ly * cosB)) };
    };

    const bool wantLine = spec.geometry == LabelGeometry::Line;

    for (const mvt::Feature& feature : layer->features)
    {
        const bool isPoint = feature.type == mvt::GeomType::Point;
        const bool isLine = feature.type == mvt::GeomType::LineString;
        if (feature.rings.empty() || (wantLine ? !isLine : !isPoint))
        {
            continue;
        }

        // GEOMETRY FIRST, STRINGS LAST.
        //
        // A viewport holds a few dozen labels; the tiles behind it hold tens of
        // thousands of road features. Everything below is arranged to reject a
        // feature using integer arithmetic before it costs a string read or an
        // allocation.
        //
        // Worth a number before this is cited as an optimisation: against the
        // real SoCal archive the whole label pass is ~2.8 ms a frame for ~450
        // candidates. This ordering is what keeps that flat as the candidate
        // list grows with zoom, not a fix for a measured problem.
        if (!wantLine)
        {
            if (feature.rings.front().empty())
            {
                continue;
            }
            const ScreenPoint at = toScreen(feature.rings.front().front());
            if (!viewport.contains(QPointF(at.x, at.y)))
            {
                continue;
            }
            const std::string text = layer->attributeText(feature, "name:latin");
            if (text.empty())
            {
                continue;
            }
            const LabelRank rank = spec.priority(*layer, feature);
            candidates.push_back(Candidate { QString::fromStdString(text), at, 0.0,
                                             spec.oneLabelPerName, rank.tier,
                                             rank.magnitude });
            continue;
        }

        // A line label goes at the MIDDLE OF THE LONGEST PART, measured on
        // screen.
        //
        // The longest part rather than the first: MVT clips a road into as many
        // parts as it has crossings of the tile edge, and the first is as often
        // as not a two-metre stub in a corner. Measured on screen rather than in
        // tile units so the choice does not change with zoom.
        //
        // This is deliberately NOT curved text following the road. That wants a
        // glyph atlas and per-character placement; a horizontal name at the
        // middle of the run is most of the value for none of that, and it stays
        // readable at every bearing -- which is the same reason the whole label
        // pass is upright.
        // Measured in TILE-LOCAL units and converted once at the end, rather
        // than projecting every point.
        //
        // The tile-to-screen transform is a similarity -- a uniform scale, a
        // rotation and a translation -- so it multiplies every length by
        // `scale` and changes no ratio. Arc length, the halfway point along it,
        // and which part is longest are therefore all the same answers computed
        // either side of it, and only the anchor has to be projected.
        //
        // This is not a micro-optimisation. The label pass runs on EVERY paint,
        // and `transportation_name` at z14 is hundreds of features of tens of
        // points each across a dozen tiles: projecting them all, into a vector
        // allocated per ring, cost more than the entire GPU frame it is drawn
        // over.
        double bestLocalLength = 0.0;
        mvt::Point bestAnchorLocal {};
        bool haveAnchor = false;

        for (const auto& ring : feature.rings)
        {
            if (ring.size() < 2)
            {
                continue;
            }

            double length = 0.0;
            for (std::size_t i = 1; i < ring.size(); ++i)
            {
                length += std::hypot(double(ring[i].x) - double(ring[i - 1].x),
                                     double(ring[i].y) - double(ring[i - 1].y));
            }
            if (length <= bestLocalLength)
            {
                continue;
            }

            // Walk to the halfway point by arc length.
            const double half = length / 2.0;
            double travelled = 0.0;
            mvt::Point anchor = ring.front();
            for (std::size_t i = 1; i < ring.size(); ++i)
            {
                const double dx = double(ring[i].x) - double(ring[i - 1].x);
                const double dy = double(ring[i].y) - double(ring[i - 1].y);
                const double segment = std::hypot(dx, dy);
                if (travelled + segment >= half)
                {
                    const double t = segment > 0.0 ? (half - travelled) / segment : 0.0;
                    anchor = mvt::Point { std::int32_t(std::lround(double(ring[i - 1].x) + (dx * t))),
                                          std::int32_t(std::lround(double(ring[i - 1].y) + (dy * t))) };
                    break;
                }
                travelled += segment;
                anchor = ring[i];
            }

            bestLocalLength = length;
            bestAnchorLocal = anchor;
            haveAnchor = true;
        }

        if (!haveAnchor || bestLocalLength <= 0.0)
        {
            continue;
        }

        const ScreenPoint at = toScreen(bestAnchorLocal);
        if (!viewport.contains(QPointF(at.x, at.y)))
        {
            continue;
        }

        // A road too short on screen to hold any name at all is rejected here,
        // before a name is even read. The exact test against the rendered text
        // width still happens at placement.
        constexpr double kShortestWorthNaming = 24.0;
        const double spanPx = bestLocalLength * scale;
        if (spanPx < kShortestWorthNaming)
        {
            continue;
        }

        // name:latin, not name. This archive's tilemaker config emits only the
        // latin field, and reading `name` returns an empty string for every
        // place -- a map with no labels and no error anywhere. map_build writes
        // BOTH spellings for exactly this reason, so the track and road layers
        // are safe either way.
        std::string text = layer->attributeText(feature, "name:latin");
        if (text.empty())
        {
            // A numbered route with no name still has something to say, and
            // map_build emits it into this layer for exactly that reason.
            text = layer->attributeText(feature, "ref");
        }
        if (text.empty())
        {
            continue;
        }

        const LabelRank rank = spec.priority(*layer, feature);
        candidates.push_back(Candidate { QString::fromStdString(text), at, spanPx,
                                         spec.oneLabelPerName, rank.tier, rank.magnitude });
    }
}

} // namespace

// map_rules writes a `rank` on every label point, LOW meaning important:
// country 0, state 1, city 2, town 3, village 4, hamlet 5, suburb 6,
// neighbourhood 7, locality 8 (libs/map_rules/src/classification.cpp).
//
// Preferred over the class name because it is the tiler's own ordering and
// stays right when a class is added upstream. The class is the fallback, for
// archives built before rank was written -- the bench archive is one, which is
// why this cannot simply require the attribute.
constexpr std::int64_t kMaxPlaceRank = 8;

int tierForRank(std::int64_t rank)
{
    return kPlaceTierStep * int(kMaxPlaceRank - std::clamp(rank, std::int64_t { 0 }, kMaxPlaceRank));
}

LabelRank placeRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    LabelRank out;

    const std::optional<double> rank = attributeNumber(layer, feature, "rank");
    out.tier = rank.has_value() ? tierForRank(std::int64_t(*rank))
                                : placePriority(layer.attributeText(feature, "class"));

    const std::optional<double> population = attributeNumber(layer, feature, "population");
    if (population.has_value() && *population > 0.0)
    {
        out.magnitude = std::uint32_t(std::clamp(*population, 0.0, 1.0e8));

        // The city/town line is drawn by local convention and moves by an order
        // of magnitude between countries, so a large town must be able to
        // outrank a small city rather than lose to it on the tag alone.
        //
        // The SAME thresholds map_rules already uses to promote a place's
        // minZoom (classification.cpp). Promoting the zoom but not the rank is
        // what produced a 400 000-strong town drawn from z6 and then labelled
        // beneath a city of 3 000.
        //
        // Upward only, for map_rules' reason: a city tagged with a small
        // population is far more often a stale tag than a tiny city.
        const int cityTier = tierForRank(2);
        const int townTier = tierForRank(3);
        if (out.magnitude >= 200'000)
        {
            out.tier = std::max(out.tier, cityTier);
        }
        else if (out.magnitude >= 50'000)
        {
            out.tier = std::max(out.tier, townTier);
        }
    }

    return out;
}

LabelRank trackRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // Between a town and a city, which is the HALF step the doubled tier scale
    // exists to express. A circuit is a landmark worth seeing from a distance,
    // and it is the reason the driver is looking at this part of the map -- but
    // it must not push a city name off a country view.
    LabelRank out;
    out.tier = tierForRank(3) + 1;

    // map_build writes a length-derived rank on every circuit: 0 for the
    // longest, 20 for the shortest (tools/map_build/tracks.cpp). Inverted here
    // because magnitude sorts high-first, and used rather than ignored so that
    // where two circuits collide the bigger one keeps its name -- the sense is
    // easy to invert and the result is a map that labels the kart track and
    // hides Spa.
    constexpr double kMaxTrackRank = 20.0;
    const std::optional<double> rank = attributeNumber(layer, feature, "rank");
    out.magnitude =
        std::uint32_t(kMaxTrackRank - std::clamp(rank.value_or(kMaxTrackRank), 0.0, kMaxTrackRank));
    return out;
}

LabelRank roadRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // Between a neighbourhood and a locality.
    //
    // Below every settlement worth the name, because a street name must not
    // push a town off the map -- and above `locality`, because at the zooms a
    // road label appears at, the street you are on is worth more than the name
    // of a road junction three miles away.
    LabelRank out;
    out.tier = tierForRank(kMaxPlaceRank) + 1;

    // Among roads, the bigger road wins the collision. roadPriority() is the
    // tessellator's own ladder -- motorway 4, trunk/primary 3, secondary 2,
    // minor 1, rail 5 -- reused rather than restated so the layer a road is
    // DRAWN in and the weight its name carries cannot drift apart.
    out.magnitude = std::uint32_t(roadPriority(layer.attributeText(feature, "class")));
    return out;
}

// The fallback for an archive whose label points carry no `rank`. Kept in step
// with map_rules' own table (classification.cpp) so that the two agree about
// which is the bigger place, and expressed through tierForRank() rather than as
// its own ladder of literals so they cannot drift apart.
//
// Returns a value on the same doubled scale placeLayerPriority() uses; only the
// ORDER is meaningful, never the number.
int placePriority(std::string_view className)
{
    if (className == "country")
    {
        return tierForRank(0);
    }
    if (className == "state" || className == "province")
    {
        return tierForRank(1);
    }
    if (className == "city")
    {
        return tierForRank(2);
    }
    if (className == "town")
    {
        return tierForRank(3);
    }
    if (className == "village")
    {
        return tierForRank(4);
    }
    if (className == "hamlet")
    {
        return tierForRank(5);
    }
    if (className == "suburb" || className == "quarter")
    {
        return tierForRank(6);
    }
    if (className == "neighbourhood")
    {
        return tierForRank(7);
    }
    // Everything unlisted, `locality` included, sorts last rather than
    // vanishing: an unknown class is a place we have no opinion about, not a
    // place that is not there.
    return tierForRank(kMaxPlaceRank);
}

void LabelCache::touch(const QString& text)
{
    const auto at = std::find(mOrder.begin(), mOrder.end(), text);
    if (at != mOrder.end())
    {
        mOrder.erase(at);
    }
    mOrder.append(text);
}

const QRectF& LabelCache::measure(const QString& text, const QFont& font)
{
    const QString key = font.key();
    if (key != mMeasureKey)
    {
        mMeasureKey = key;
        mMeasured.clear();
    }

    if (const auto found = mMeasured.constFind(text); found != mMeasured.constEnd())
    {
        return *found;
    }

    // Bounded the same way the rendered cache is, and for the same reason: a
    // long drive passes through a great many street names.
    constexpr int kMaxMeasured = 4096;
    if (mMeasured.size() >= kMaxMeasured)
    {
        mMeasured.clear();
    }

    const QFontMetricsF metrics(font);
    return *mMeasured.insert(text, metrics.boundingRect(text));
}

const LabelCache::Entry& LabelCache::entryFor(const QString& text, const QFont& font,
                                              double haloWidth, const QColor& haloColour,
                                              const QColor& textColour, double devicePixelRatio)
{
    // Everything baked into the image is part of the key. Getting this wrong
    // would hand back a label in the previous style, which reads as the style
    // change not having applied.
    const QString key = font.key() + QChar('|') + QString::number(haloWidth) + QChar('|') +
                        haloColour.name(QColor::HexArgb) + QChar('|') +
                        textColour.name(QColor::HexArgb) + QChar('|') +
                        QString::number(devicePixelRatio);
    if (key != mKey)
    {
        mKey = key;
        mEntries.clear();
        mOrder.clear();
    }

    if (const auto found = mEntries.constFind(text); found != mEntries.constEnd())
    {
        touch(text);
        return *found;
    }

    // A long drive through many named places would otherwise grow this without
    // bound.
    //
    // EVICTED ONE AT A TIME, least recently used first, rather than cleared.
    // Clearing is a cliff: a label costs 0.88 ms to render, so a viewport
    // holding forty of them pays about 35 ms -- two dropped frames -- in the
    // single frame that crosses the threshold, and it pays it again on any
    // frame that crosses back. Evicting the oldest costs one render for one
    // label that had left the screen anyway.
    //
    // The list scan is O(n) in 512 entries and runs a few dozen times a frame.
    // That is thousands of pointer comparisons against a 0.88 ms render, so
    // the bookkeeping an O(1) LRU would need buys nothing measurable.
    constexpr int kMaxEntries = 512;
    while (mEntries.size() >= kMaxEntries && !mOrder.isEmpty())
    {
        mEntries.remove(mOrder.takeFirst());
    }
    mOrder.append(text);

    const QFontMetricsF metrics(font);

    Entry entry;
    entry.bounds = metrics.boundingRect(text);

    // Room for the halo, which straddles the outline, plus a pixel for the
    // antialiasing to fade into. Without it the stroke is clipped at the edges
    // and the label looks bitten.
    //
    // A WHOLE number of pixels, because the blit position is rounded to whole
    // pixels too -- see below. A fractional offset would make Qt resample the
    // image and the text would come out soft.
    const double pad = std::ceil(haloWidth / 2.0) + 2.0;
    entry.offset = QPointF(-pad, -pad);

    const double width = entry.bounds.width() + (2.0 * pad);
    const double height = entry.bounds.height() + (2.0 * pad);
    const double ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;

    entry.image = QImage(QSize(static_cast<int>(std::ceil(width * ratio)),
                               static_cast<int>(std::ceil(height * ratio))),
                         QImage::Format_ARGB32_Premultiplied);
    entry.image.setDevicePixelRatio(ratio);
    entry.image.fill(Qt::transparent);

    QPainter into(&entry.image);
    into.setRenderHint(QPainter::Antialiasing, true);
    into.setRenderHint(QPainter::TextAntialiasing, true);

    // Place the BOUNDING BOX, not the baseline, at (pad, pad) -- the image was
    // sized from that box, and it is the box paintLabels() centres and
    // collides. In practice boundingRect()'s y() is exactly -ascent(), so the
    // vertical term equals the ascent anchoring an earlier version used; its
    // x() is the left bearing, up to a pixel and a half, which that version
    // dropped -- every label sat a hair left of the box it had claimed.
    // Written as the box-to-box mapping rather than baseline arithmetic so it
    // stays right even where a glyph overshoots the font's metrics.
    QPainterPath glyphs;
    glyphs.addText(pad - entry.bounds.x(), pad - entry.bounds.y(), font, text);

    // Halo by stroking the glyph outlines rather than drawing the text several
    // times at offsets: one path, one stroke, and the halo is even on every
    // side. The offset trick leaves the corners thin.
    if (haloWidth > 0.0)
    {
        QPen halo(haloColour);
        halo.setWidthF(haloWidth);
        halo.setJoinStyle(Qt::RoundJoin);
        into.setPen(halo);
        into.setBrush(Qt::NoBrush);
        into.drawPath(glyphs);
    }

    into.setPen(Qt::NoPen);
    into.fillPath(glyphs, textColour);
    into.end();

    return *mEntries.insert(text, std::move(entry));
}

LabelStats paintLabels(QPainter& painter, const Projection& projection,
                       const std::vector<LabelTile>& tiles, const MapStyle_t& style,
                       LabelCache& cache)
{
    LabelStats stats;
    if (!style.show_labels)
    {
        return stats;
    }

    // Collected first, placed second. A label's position depends on which
    // labels were already accepted, and the order they are found in is tile
    // decode order -- which is not an order anybody chose.
    std::vector<Candidate> candidates;

    // Generous enough that a label whose anchor is just off screen but whose
    // text would reach onto it is still considered.
    const QRectF viewport(0.0, 0.0, projection.viewportWidth(), projection.viewportHeight());
    const QRectF gatherBounds = viewport.adjusted(-256.0, -64.0, 256.0, 64.0);

    for (const LabelTile& entry : tiles)
    {
        if (!entry.tile)
        {
            continue;
        }
        for (const LabelLayerSpec& spec : kLabelLayers)
        {
            if (spec.enabled != nullptr && !spec.enabled(style, projection.camera().zoom))
            {
                continue;
            }
            gatherLabels(entry, spec, projection, gatherBounds, candidates);
        }
    }

    // Tier first, then size within a tier. STABLE still, so that two places of
    // the same kind and the same population keep tile decode order rather than
    // swapping between frames -- a label that flickers as you pan is worse than
    // one that loses consistently.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.priority != b.priority)
                         {
                             return a.priority > b.priority;
                         }
                         if (a.magnitude != b.magnitude)
                         {
                             return a.magnitude > b.magnitude;
                         }
                         // Longest on-screen run first, so that the one label a
                         // road gets lands on its longest visible stretch
                         // rather than on whichever tile decoded first.
                         return a.spanPx > b.spanPx;
                     });

    QFont font = painter.font();
    if (!style.label_font.empty())
    {
        font.setFamily(QString::fromStdString(style.label_font));
    }
    font.setPointSizeF(double(style.label_size));
    painter.setFont(font);
    const QColor textColour = qt_helpers::toQColor(style.label_text);
    const QColor haloColour = qt_helpers::toQColor(style.label_halo);

    // A linear scan against accepted boxes. A viewport holds tens of labels,
    // not thousands, so a grid would cost more to maintain than it saves.
    std::vector<QRectF> taken;
    // Names already placed, for the layers that ask for one label each. A road
    // crosses every tile it passes through and each tile carries the whole
    // name, so without this a single street is labelled a dozen times across
    // one viewport.
    //
    // The candidates are already sorted, so the FIRST time a name is seen is
    // its best placement -- biggest road first, and within a road the part with
    // the longest on-screen run.
    QSet<QString> namesPlaced;

    for (const Candidate& candidate : candidates)
    {
        // MEASURED, not rendered. Rendering costs 0.88 ms a label; measuring is
        // a font-metrics lookup. Only the labels that survive every test below
        // are worth pixels -- and with road names in the mix, the candidate
        // list is tens of thousands long while the placed set is a few dozen.
        const QRectF& text = cache.measure(candidate.text, font);
        const QRectF box(candidate.at.x - (text.width() / 2.0),
                         candidate.at.y - (text.height() / 2.0), text.width(), text.height());

        if (!viewport.intersects(box))
        {
            continue;
        }

        // A name wider than the thing it names reads as text floating over the
        // map rather than as a label on a road. Cheaper than it looks: this is
        // the check that keeps the two-metre stubs MVT leaves in tile corners
        // from each claiming a label.
        if (candidate.spanPx > 0.0 && text.width() > candidate.spanPx)
        {
            ++stats.suppressed;
            continue;
        }

        if (candidate.oneLabelPerName && namesPlaced.contains(candidate.text))
        {
            continue;
        }

        // Padded, so two labels never end up touching -- text that merely
        // avoids overlapping still reads as one run of words. Half the spacing
        // either side, and half again vertically: lines crowd sooner than
        // columns do.
        const double padX = double(style.label_spacing);
        const double padY = padX / 2.0;
        const QRectF padded = box.adjusted(-padX, -padY, padX, padY);
        if (std::any_of(taken.begin(), taken.end(),
                        [&](const QRectF& other) { return other.intersects(padded); }))
        {
            ++stats.suppressed;
            continue;
        }
        taken.push_back(padded);
        if (candidate.oneLabelPerName)
        {
            namesPlaced.insert(candidate.text);
        }

        // Only NOW is it worth pixels.
        const LabelCache::Entry& entry =
            cache.entryFor(candidate.text, font, style.label_halo_width, haloColour, textColour,
                           painter.device() != nullptr ? painter.device()->devicePixelRatioF()
                                                       : 1.0);

        // Snapped to whole pixels. Blitting at a fractional position makes Qt
        // resample, and resampled text is visibly soft; half a pixel of
        // placement error on a place name is not.
        const QPointF where = box.topLeft() + entry.offset;
        painter.drawImage(QPointF(std::round(where.x()), std::round(where.y())), entry.image);

        ++stats.placed;
    }

    return stats;
}

} // namespace map_widget
