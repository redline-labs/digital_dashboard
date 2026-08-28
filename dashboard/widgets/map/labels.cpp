// SPDX-License-Identifier: GPL-3.0-or-later
//
// Placing and drawing labels. The GUI THREAD's half of the label pass.
//
// Its other half is label_candidates.cpp, which turns a decoded tile into the
// camera-free candidates this file places. The split is along the thread
// boundary: nothing here runs on a decode worker, and nothing there knows what
// a camera or a painter is.
#include "map/labels.h"

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
#include <QTransform>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
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
    // The road's run, projected to screen, as a range into the frame's arena.
    // Empty for a point label, which is placed at a point and drawn upright.
    std::uint32_t runBegin { 0 };
    std::uint32_t runCount { 0 };
};

// Which source layers carry labels, and how each ranks its own.
//
// A TABLE rather than a hardcoded layer(), because there is more than one now:
// the basemap's `place` and the track archive's `track_label`, which arrive in
// the same viewport from two different archives. They compete for the same
// screen space and are placed by one pass, which is the whole reason the
// candidates are gathered before any of them is drawn.
bool roadLabelsEnabled(const MapStyle_t& style, double zoom)
{
    return style.show_road_labels && zoom >= double(style.detail.road_label);
}

bool waterLabelsEnabled(const MapStyle_t& style, double zoom)
{
    return style.show_water_labels && zoom >= double(style.detail.water_label);
}

// A road too short on screen to hold any name at all is rejected before a
// candidate is even built. The exact test against the rendered text width
// still happens at placement.
constexpr double kShortestWorthNaming = 24.0;

} // namespace

void LabelCache::touch(const QString& text)
{
    const auto at = std::find(mOrder.begin(), mOrder.end(), text);
    if (at != mOrder.end())
    {
        mOrder.erase(at);
    }
    mOrder.append(text);
}

double LabelCache::advanceFor(QChar ch, const QFont& font)
{
    refont(font);

    const char32_t code = ch.unicode();
    if (const auto found = mAdvances.constFind(code); found != mAdvances.constEnd())
    {
        return *found;
    }

    // Never evicted: this is an alphabet, and an alphabet does not grow with
    // the driving the way a list of street names does.
    const QFontMetricsF metrics(font);
    return *mAdvances.insert(code, metrics.horizontalAdvance(QString(ch)));
}

const QRectF& LabelCache::measure(const QString& text, const QFont& font)
{
    refont(font);

    if (const auto found = mMeasured.constFind(text); found != mMeasured.constEnd())
    {
        return *found;
    }

    // Bounded the same way the rendered cache is, and for the same reason: a
    // long drive passes through a great many street names.
    //
    // CLEARED, not LRU-evicted, deliberately. measure() runs per candidate
    // per frame -- tens of thousands of calls with road labels on -- and an
    // O(n) touch on every hit costs more every single frame than this clear
    // costs once; the re-measures after it are font-metrics lookups, not the
    // 0.88 ms renders the entry cache protects.
    constexpr int kMaxMeasured = 4096;
    if (mMeasured.size() >= kMaxMeasured)
    {
        mMeasured.clear();
    }

    const QFontMetricsF metrics(font);
    return *mMeasured.insert(text, metrics.boundingRect(text));
}

void LabelCache::rekey(const StyleKey& key)
{
    // Everything baked into the images is part of the key. Getting this wrong
    // would hand back a label in the previous style, which reads as the style
    // change not having applied.
    //
    // ONE key for both image tiers. A glyph and a whole string are rendered
    // from the same font in the same two colours, so a change that invalidates
    // one invalidates the other; keeping two keys would only create the state
    // where half the alphabet is stale.
    if (key == mKey)
    {
        return;
    }
    mKey = key;
    mEntries.clear();
    mOrder.clear();
    mGlyphs.clear();
}

void LabelCache::refont(const QFont& font)
{
    if (mHaveMeasureFont && font == mMeasureFont)
    {
        return;
    }
    mMeasureFont = font;
    mHaveMeasureFont = true;
    mMeasured.clear();
    mAdvances.clear();
}

const LabelCache::Glyph& LabelCache::glyphFor(QChar ch, const QFont& font, double haloWidth,
                                              const QColor& haloColour, const QColor& textColour,
                                              double devicePixelRatio)
{
    rekey(StyleKey { font, haloWidth, haloColour.rgba(), textColour.rgba(), devicePixelRatio });

    const char32_t key = ch.unicode();
    if (const auto found = mGlyphs.constFind(key); found != mGlyphs.constEnd())
    {
        return *found;
    }

    // NOT evicted, unlike the string tier. That one is bounded by how many
    // place names a drive goes past; this one is bounded by how many distinct
    // characters a map's names are spelled with, which is an alphabet and does
    // not grow with the driving.
    const QFontMetricsF metrics(font);
    const QString one(ch);

    Glyph glyph;
    glyph.advance = metrics.horizontalAdvance(one);

    // The ink box, relative to the pen on the baseline. y() is negative for
    // anything with an ascender, which is why the pen is recovered from the
    // box rather than assumed to be at its top left.
    const QRectF ink = metrics.boundingRect(one);
    const double pad = std::ceil(haloWidth / 2.0) + 2.0;
    glyph.pen = QPointF(pad - ink.x(), pad - ink.y());

    const double ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const QSize size(int(std::ceil((ink.width() + (2.0 * pad)) * ratio)),
                     int(std::ceil((ink.height() + (2.0 * pad)) * ratio)));

    // A space, and anything else with no ink, has a real advance and an empty
    // box. Null images draw nothing and the walk steps over them.
    if (size.width() <= 0 || size.height() <= 0)
    {
        return *mGlyphs.insert(key, std::move(glyph));
    }

    QPainterPath outline;
    outline.addText(glyph.pen, font, one);

    if (haloWidth > 0.0)
    {
        glyph.halo = QImage(size, QImage::Format_ARGB32_Premultiplied);
        glyph.halo.setDevicePixelRatio(ratio);
        glyph.halo.fill(Qt::transparent);
        QPainter into(&glyph.halo);
        into.setRenderHint(QPainter::Antialiasing, true);
        // Stroked, not drawn several times at offsets: one path, one stroke,
        // and the halo is even on every side. The offset trick leaves the
        // corners thin.
        QPen pen(haloColour);
        pen.setWidthF(haloWidth);
        pen.setJoinStyle(Qt::RoundJoin);
        into.setPen(pen);
        into.setBrush(Qt::NoBrush);
        into.drawPath(outline);
    }

    glyph.fill = QImage(size, QImage::Format_ARGB32_Premultiplied);
    glyph.fill.setDevicePixelRatio(ratio);
    glyph.fill.fill(Qt::transparent);
    {
        QPainter into(&glyph.fill);
        into.setRenderHint(QPainter::Antialiasing, true);
        into.setPen(Qt::NoPen);
        into.fillPath(outline, textColour);
    }

    return *mGlyphs.insert(key, std::move(glyph));
}

const LabelCache::Entry& LabelCache::entryFor(const QString& text, const QFont& font,
                                              double haloWidth, const QColor& haloColour,
                                              const QColor& textColour, double devicePixelRatio)
{
    rekey(StyleKey { font, haloWidth, haloColour.rgba(), textColour.rgba(), devicePixelRatio });

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

// ---------------------------------------------------------------- curved text

// How far the text may turn, in total, within one window of its own length.
//
// MapLibre's `text-max-angle` default, and for its reason: a label has to be
// rejected where the road kinks under it, but a long gentle curve is exactly
// what this feature is FOR and must not be rejected. Summing turn over a
// sliding window separates those two; a per-glyph threshold cannot, because it
// passes a spiral made of shallow steps and fails a single kink in an
// otherwise straight road.
constexpr double kMaxLabelTurn = 45.0 * (std::numbers::pi / 180.0);

// The window that turn is summed over, as a fraction of the font's em.
// MapLibre uses three fifths of a glyph's width; the same idea, expressed in
// the one length the font hands us directly.
constexpr double kTurnWindowEms = 0.6;

// How many glyphs share one collision box.
//
// One box per label is hopeless for text on a diagonal -- the axis-aligned
// hull of a rotated word is mostly empty space, and it evicts neighbours that
// never touched it. One box per GLYPH is exact but multiplies the collision
// scan by the length of every name. Four is the knee: the hull of four
// characters is tight enough that the empty corners are smaller than the
// padding `label_spacing` already adds.
constexpr int kGlyphsPerCollisionBox = 4;

// How far a character may sit off the straight line through the label's two
// ends before the label counts as curved, in pixels.
//
// The question this answers is "would drawing this name as one straight image
// look different?", so it is asked in PIXELS and not in degrees. An angle
// threshold gets it wrong in both directions: half a degree of bend is
// invisible on a short name and obvious on a long one, and real roads wander
// by a fraction of a degree constantly without ever looking bent.
//
// It matters a great deal which side of this a label falls. A straight label
// is one blit of a cached image; a curved one is a rotated blit per character,
// twice over for the halo, and a rotated blit measured about 7 us. Asked in
// degrees at half a degree, two thirds of the road labels in Irvine -- a grid
// city of straight streets -- came out "curved" and the label pass cost 3.4 ms
// a frame. Asked in pixels, almost none of them do.
constexpr double kStraightEnoughPx = 0.75;

// A ceiling on how many times one run may repeat its name.
//
// Only ever reached by a pathological combination -- a very short name on a
// very long run with the repeat distance turned down -- but without it a
// single feature could claim an unbounded number of collision boxes and the
// per-frame scan is quadratic in those.
constexpr int kMaxRepeatsPerRun = 16;

// One character, placed on the road.
struct PlacedGlyph
{
    QChar ch;
    // The centre of the character's own advance, on the text's centre line --
    // not its baseline. Text is centred ACROSS the road, so the road runs
    // through the middle of the letters rather than under their feet.
    QPointF at;
    double angle { 0.0 };
    double advance { 0.0 };
};

// A polyline, walked by arc length.
//
// Holds no geometry of its own: the run lives in the frame's arena and this is
// the cursor over it. Distances are screen pixels, because the run was
// projected before any of this ran -- which is what makes the glyph spacing
// right rather than merely proportional.
class RunWalker
{
  public:
    RunWalker(const ScreenPoint* points, std::size_t count) : mPoints(points), mCount(count)
    {
        mLength = 0.0;
        for (std::size_t i = 1; i < mCount; ++i)
        {
            mLength += std::hypot(mPoints[i].x - mPoints[i - 1].x,
                                  mPoints[i].y - mPoints[i - 1].y);
        }
    }

    double length() const { return mLength; }

    struct At
    {
        QPointF point;
        double angle { 0.0 };
    };

    // Where the run is `distance` along, and which way it points there.
    //
    // `reversed` walks from the far end with pi added to the angle, which is
    // how a label that would read right-to-left is turned around: the same
    // road, travelled the other way, so the letters come out in order and the
    // right way up. See placeAlongRun().
    std::optional<At> at(double distance, bool reversed) const
    {
        if (mCount < 2 || distance < 0.0 || distance > mLength)
        {
            return std::nullopt;
        }
        const double target = reversed ? mLength - distance : distance;

        double travelled = 0.0;
        for (std::size_t i = 1; i < mCount; ++i)
        {
            const double dx = mPoints[i].x - mPoints[i - 1].x;
            const double dy = mPoints[i].y - mPoints[i - 1].y;
            const double segment = std::hypot(dx, dy);
            if (segment <= 0.0)
            {
                continue;
            }
            if (travelled + segment >= target)
            {
                const double t = (target - travelled) / segment;
                double angle = std::atan2(dy, dx);
                if (reversed)
                {
                    angle += std::numbers::pi;
                }
                return At { QPointF(mPoints[i - 1].x + (dx * t), mPoints[i - 1].y + (dy * t)),
                            angle };
            }
            travelled += segment;
        }

        // Ran off the end through floating-point drift rather than through
        // being asked for something off the run. Answer with the last segment
        // rather than dropping a label for a rounding error.
        const double dx = mPoints[mCount - 1].x - mPoints[mCount - 2].x;
        const double dy = mPoints[mCount - 1].y - mPoints[mCount - 2].y;
        double angle = std::atan2(dy, dx);
        if (reversed)
        {
            angle += std::numbers::pi;
        }
        const ScreenPoint& endPoint = reversed ? mPoints[0] : mPoints[mCount - 1];
        return At { QPointF(endPoint.x, endPoint.y), angle };
    }

  private:
    const ScreenPoint* mPoints;
    std::size_t mCount;
    double mLength { 0.0 };
};

// The signed difference between two angles, wrapped into [-pi, pi].
double angleDelta(double from, double to)
{
    double delta = std::fmod((to - from) + (3.0 * std::numbers::pi), 2.0 * std::numbers::pi);
    return delta - std::numbers::pi;
}

// A label laid out on a road.
struct PlacedLabel
{
    std::vector<PlacedGlyph> glyphs;
    // Where the middle of the text sits, and which way it points there.
    QPointF centre;
    // Set when every character came out at the same angle -- which is to say
    // the road is straight under the name, which most roads at most zooms are.
    //
    // This is the difference between one blit and one per character, and it is
    // worth having: laying a name out character by character measured 4.7 ms a
    // frame against 0.58 ms for the cached whole-string image it replaced,
    // because a rotated blit of a small image is not free and a name is a
    // dozen of them, twice over for the halo. Curved text costs that; straight
    // text must not, and nearly all text is straight.
    std::optional<double> uniformAngle;
};

// Lay `text` along `walker`, centred on the run's midpoint.
//
// Returns nothing when the label does not belong on this road: it is longer
// than the road, or the road turns too hard under it. Both are rejections, not
// fallbacks -- a name that does not fit its road drawn anyway is the floating
// text this pass has always refused to draw.
std::optional<PlacedLabel> placeAlongRun(const QString& text, const RunWalker& walker,
                                         LabelCache& cache, const QFont& font, double emSize,
                                         double start)
{
    if (text.isEmpty())
    {
        return std::nullopt;
    }

    // ADVANCES, not pixels. Every road name on screen is laid out here --
    // hundreds a frame -- and only the couple of dozen that survive collision
    // are ever drawn. Rendering the alphabet to find out how wide a name is
    // costs the whole label budget; see LabelCache::advanceFor.
    std::vector<PlacedGlyph> glyphs;
    glyphs.reserve(std::size_t(text.size()));
    double width = 0.0;
    for (const QChar ch : text)
    {
        const double advance = cache.advanceFor(ch, font);
        glyphs.push_back(PlacedGlyph { ch, {}, 0.0, advance });
        width += advance;
    }

    // The exact test the approximate one at gather time stands in for: a name
    // wider than the road it names reads as text floating over the map.
    if (width <= 0.0 || width > walker.length())
    {
        return std::nullopt;
    }
    if (start < 0.0 || start + width > walker.length())
    {
        return std::nullopt;
    }

    // WHICH WAY ROUND. Decided once, from where the first and last characters
    // land, and never per glyph -- flipping glyphs individually scrambles the
    // word. If the text would run right-to-left across the screen, the same
    // road is walked from the other end instead, which puts the letters in
    // order and the right way up at every bearing.
    //
    // Only the two ends are needed to answer it, so it costs two walks and not
    // a whole layout. MapLibre decides it the same way and for the same
    // reason.
    const std::optional<RunWalker::At> firstEnd = walker.at(start + (glyphs.front().advance / 2.0),
                                                            false);
    const std::optional<RunWalker::At> lastEnd =
        walker.at(start + width - (glyphs.back().advance / 2.0), false);
    if (!firstEnd.has_value() || !lastEnd.has_value())
    {
        return std::nullopt;
    }
    const bool reversed = firstEnd->point.x() > lastEnd->point.x();

    // Where along the run each character's centre sits, kept so the turn
    // window below can be measured in arc length rather than in characters --
    // a window of "three glyphs" means different things for an I and a W.
    std::vector<double> centres(glyphs.size(), 0.0);

    double travelled = start;
    for (std::size_t i = 0; i < glyphs.size(); ++i)
    {
        centres[i] = travelled + (glyphs[i].advance / 2.0);
        const std::optional<RunWalker::At> at = walker.at(centres[i], reversed);
        if (!at.has_value())
        {
            return std::nullopt;
        }
        glyphs[i].at = at->point;
        glyphs[i].angle = at->angle;
        travelled += glyphs[i].advance;
    }

    // The curvature test, over a window that slides along the label: the sum
    // of the turns between consecutive characters, for every stretch of road
    // one window long. A long gentle curve keeps every window's sum small; a
    // kink puts all of its turn inside one window and fails.
    const double window = std::max(1.0, kTurnWindowEms * emSize);
    double turn = 0.0;
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < glyphs.size(); ++i)
    {
        turn += std::abs(angleDelta(glyphs[i - 1].angle, glyphs[i].angle));
        while (oldest + 1 < i && centres[i] - centres[oldest] > window)
        {
            turn -= std::abs(angleDelta(glyphs[oldest].angle, glyphs[oldest + 1].angle));
            ++oldest;
        }
        if (turn > kMaxLabelTurn)
        {
            return std::nullopt;
        }
    }

    PlacedLabel out;

    // Straight, if every character sits within a fraction of a pixel of the
    // line through the two ends -- which is exactly the line a single blit
    // would put them on.
    //
    // Measured against that chord rather than between neighbours: a long
    // shallow arc turns imperceptibly at every step and leaves its ends well
    // off the chord, and drawing that as one image would visibly lift the name
    // off the road at both ends.
    const QPointF from = glyphs.front().at;
    const QPointF to = glyphs.back().at;
    const QPointF along = to - from;
    const double span = std::hypot(along.x(), along.y());
    double worst = 0.0;
    if (span > 0.0)
    {
        for (const PlacedGlyph& glyph : glyphs)
        {
            const QPointF off = glyph.at - from;
            // Distance to the chord: the cross product over its length.
            worst = std::max(worst, std::abs(((off.x() * along.y()) - (off.y() * along.x()))) /
                                        span);
        }
    }
    if (worst <= kStraightEnoughPx)
    {
        // The chord's own angle, not the first character's: over a very
        // slightly bent run the chord is what the single image will lie on.
        out.uniformAngle = std::atan2(along.y(), along.x());
    }

    const std::optional<RunWalker::At> middle = walker.at(start + (width / 2.0), reversed);
    out.centre = middle.has_value() ? middle->point : glyphs.front().at;
    out.glyphs = std::move(glyphs);
    return out;
}

// Lay `text` along `walker` as many times as it comfortably fits.
//
// A long road carries its name several times rather than once, so the name is
// legible wherever the driver happens to be looking rather than only at a
// midpoint that may well be off screen. `repeat` is the clear road demanded
// between one instance and the next; zero asks for a single centred label,
// which is what the map did before repeats existed.
//
// The whole set is centred on the run, so the single-label case is exactly the
// old placement and not an approximation of it.
void placeRepeatsAlongRun(const QString& text, const RunWalker& walker, LabelCache& cache,
                          const QFont& font, double emSize, double repeat,
                          std::vector<PlacedLabel>& out)
{
    out.clear();
    if (text.isEmpty())
    {
        return;
    }

    double width = 0.0;
    for (const QChar ch : text)
    {
        width += cache.advanceFor(ch, font);
    }
    if (width <= 0.0 || width > walker.length())
    {
        return;
    }

    // How many fit, each needing its own width plus a gap before the next.
    // The trailing gap is not required, hence the `+ repeat` on the numerator.
    int count = 1;
    if (repeat > 0.0)
    {
        const double stride = width + repeat;
        count = std::max(1, int(std::floor((walker.length() + repeat) / stride)));
        count = std::min(count, kMaxRepeatsPerRun);
    }

    const double stride = width + repeat;
    const double occupied = (double(count) * width) + (double(count - 1) * repeat);
    const double leading = (walker.length() - occupied) / 2.0;

    for (int i = 0; i < count; ++i)
    {
        std::optional<PlacedLabel> placed =
            placeAlongRun(text, walker, cache, font, emSize, leading + (double(i) * stride));
        if (placed.has_value())
        {
            out.push_back(std::move(*placed));
        }
    }
}

// The axis-aligned hulls a placed label claims, a few glyphs to a box.
void collisionBoxesFor(const std::vector<PlacedGlyph>& glyphs, double height,
                       std::vector<QRectF>& out)
{
    for (std::size_t i = 0; i < glyphs.size(); i += kGlyphsPerCollisionBox)
    {
        const std::size_t last = std::min(glyphs.size(), i + kGlyphsPerCollisionBox);
        QRectF box;
        for (std::size_t j = i; j < last; ++j)
        {
            // The character's own quad, turned the way the character is, then
            // reduced to the box that contains it.
            QTransform turn;
            turn.translate(glyphs[j].at.x(), glyphs[j].at.y());
            turn.rotateRadians(glyphs[j].angle);
            const QRectF local(-glyphs[j].advance / 2.0, -height / 2.0, glyphs[j].advance, height);
            const QRectF hull = turn.mapRect(local);
            box = box.isNull() ? hull : box.united(hull);
        }
        if (!box.isNull())
        {
            out.push_back(box);
        }
    }
}

// Blit a placed label: every halo, then every fill.
//
// The two passes are not an optimisation and cannot be merged. See
// LabelCache::Glyph -- one pass would let each character's halo paint over the
// previous character's text.
void drawPlacedGlyphs(QPainter& painter, const std::vector<PlacedGlyph>& glyphs,
                      LabelCache& cache, const QFont& font, double haloWidth,
                      const QColor& haloColour, const QColor& textColour, double ratio,
                      double baselineShift, std::vector<const LabelCache::Glyph*>& scratch)
{
    // Looked up once and held, rather than once per pass: the lookup builds a
    // style key and the two passes want the same entries.
    scratch.clear();
    scratch.reserve(glyphs.size());
    for (const PlacedGlyph& placed : glyphs)
    {
        scratch.push_back(
            &cache.glyphFor(placed.ch, font, haloWidth, haloColour, textColour, ratio));
    }

    for (int pass = 0; pass < 2; ++pass)
    {
        for (std::size_t i = 0; i < glyphs.size(); ++i)
        {
            const LabelCache::Glyph& glyph = *scratch[i];
            const QImage& image = pass == 0 ? glyph.halo : glyph.fill;
            if (image.isNull())
            {
                continue;
            }

            // setWorldTransform rather than save/translate/rotate/restore:
            // QPainter::save copies the entire painter state -- pen, brush,
            // clip, every render hint -- and a curved name does this once per
            // character per pass. Setting the matrix outright touches one
            // field.
            QTransform place;
            place.translate(glyphs[i].at.x(), glyphs[i].at.y());
            place.rotateRadians(glyphs[i].angle);
            // From the centre of the advance on the text's centre line, back
            // to where the pen belongs: half an advance left, and down by
            // however far the baseline sits below that centre line.
            place.translate(-glyphs[i].advance / 2.0, baselineShift);
            painter.setWorldTransform(place);
            painter.drawImage(QPointF(-glyph.pen.x(), -glyph.pen.y()), image);
        }
    }

    painter.setWorldTransform(QTransform());
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

    // The per-layer gates, evaluated once per frame -- kinds are baked on each
    // candidate at extraction, so the per-candidate test is one branch.
    const bool roadsOn = roadLabelsEnabled(style, projection.camera().zoom);
    const bool waterOn = waterLabelsEnabled(style, projection.camera().zoom);

    // Every road run this frame, projected once, end to end. One arena rather
    // than a vector per candidate: the steady repaint allocates nothing, and
    // this is the only new per-frame storage the curved labels need.
    std::vector<ScreenPoint> runs;

    for (const LabelTile& entry : tiles)
    {
        if (!entry.labels || entry.labels->empty())
        {
            continue;
        }

        // The whole per-frame cost of a tile's labels: one transform solved,
        // and per candidate a map, a bounds test and a push. Everything
        // heavier -- attribute walks, string reads, arc lengths, simplifying
        // the run -- happened once, at decode time, on the worker.
        const Projection::TileTransform transform = projection.tileTransform(entry.id);
        const double size = transform.size;

        for (const LabelCandidate& candidate : entry.labels->labels)
        {
            if (candidate.kind == LabelKind::Road && !roadsOn)
            {
                continue;
            }
            if (candidate.kind == LabelKind::Water && !waterOn)
            {
                continue;
            }
            // The tile's own axes are rotated with the map even though a place
            // name is not, so an anchor goes through the same transform the
            // GPU applies to the geometry.
            const ScreenPoint at = transform.map(candidate.x, candidate.y);
            if (!gatherBounds.contains(QPointF(at.x, at.y)))
            {
                continue;
            }
            const double spanPx = candidate.spanLocal * size;
            if (candidate.spanLocal > 0.0 && spanPx < kShortestWorthNaming)
            {
                continue;
            }

            Candidate out { candidate.text,          at,
                            spanPx,                  candidate.oneLabelPerName,
                            candidate.priority,      candidate.magnitude };

            // A line label carries its road with it. Projected here, where the
            // tile's transform is already in hand, rather than at placement
            // where it would have to be found again per candidate.
            if (candidate.pathCount >= 2)
            {
                out.runBegin = std::uint32_t(runs.size());
                out.runCount = candidate.pathCount;
                for (std::uint32_t i = 0; i < candidate.pathCount; ++i)
                {
                    const LocalPoint& point = entry.labels->path[candidate.pathBegin + i];
                    runs.push_back(transform.map(double(point.x), double(point.y)));
                }
            }

            candidates.push_back(std::move(out));
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

    const double ratio =
        painter.device() != nullptr ? painter.device()->devicePixelRatioF() : 1.0;

    const QFontMetricsF metrics(font);
    // Text is centred ACROSS the road, so the road runs through the middle of
    // the letters rather than under their feet. This is how far the baseline
    // sits below that centre line.
    const double baselineShift = (metrics.ascent() - metrics.descent()) / 2.0;
    const double emSize = metrics.height();

    // A linear scan against accepted boxes. A viewport holds tens of labels,
    // not thousands, so a grid would cost more to maintain than it saves.
    // A road label contributes several boxes rather than one -- see
    // collisionBoxesFor() -- which is still a few hundred, not thousands.
    std::vector<QRectF> taken;
    // What the candidate being tested claims. Reused rather than rebuilt, like
    // everything else on this path.
    std::vector<QRectF> boxes;
    std::vector<const LabelCache::Glyph*> glyphScratch;
    std::vector<PlacedLabel> placements;

    // Where each name has already been placed.
    //
    // A road crosses every tile it passes through and each tile carries the
    // whole name, so without this a single street is labelled a dozen times
    // over. But a long road SHOULD carry its name more than once -- just not
    // twice in the same place -- so this holds positions and rejects a repeat
    // that lands within `label_repeat_distance` of one already there, rather
    // than rejecting the name outright.
    //
    // The candidates are already sorted, so the FIRST time a name is seen is
    // its best placement -- biggest road first, and within a road the part with
    // the longest on-screen run.
    const double repeatPx = double(style.label_repeat_distance);
    QHash<QString, std::vector<QPointF>> namesPlaced;

    // Whether this name is already spoken for at this spot. With the repeat
    // distance at zero the answer is "anywhere at all", which is one label per
    // name per viewport -- what the map did before it repeated anything.
    const auto claimedNearby = [&](const QString& name, const QPointF& at) {
        const auto found = namesPlaced.constFind(name);
        if (found == namesPlaced.constEnd())
        {
            return false;
        }
        if (repeatPx <= 0.0)
        {
            return true;
        }
        return std::any_of(found->begin(), found->end(), [&](const QPointF& other) {
            return std::hypot(other.x() - at.x(), other.y() - at.y()) < repeatPx;
        });
    };

    // Does this label's claim collide with anything already standing?
    const auto collides = [&](const std::vector<QRectF>& claims) {
        return std::any_of(claims.begin(), claims.end(), [&](const QRectF& claim) {
            return std::any_of(taken.begin(), taken.end(),
                               [&](const QRectF& other) { return other.intersects(claim); });
        });
    };

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

        // Padded, so two labels never end up touching -- text that merely
        // avoids overlapping still reads as one run of words. Half the spacing
        // either side, and half again vertically: lines crowd sooner than
        // columns do.
        const double padX = double(style.label_spacing);
        const double padY = padX / 2.0;
        const auto pad = [&](std::vector<QRectF>& claims) {
            for (QRectF& claim : claims)
            {
                claim = claim.adjusted(-padX, -padY, padX, padY);
            }
        };

        // ---- a point label: one upright name, at its own anchor -----------
        if (candidate.runCount < 2)
        {
            if (candidate.oneLabelPerName && claimedNearby(candidate.text, box.center()))
            {
                continue;
            }

            boxes.clear();
            boxes.push_back(box);
            pad(boxes);
            if (collides(boxes))
            {
                ++stats.suppressed;
                continue;
            }
            taken.insert(taken.end(), boxes.begin(), boxes.end());
            if (candidate.oneLabelPerName)
            {
                namesPlaced[candidate.text].push_back(box.center());
            }

            // Only NOW is it worth pixels.
            const LabelCache::Entry& entry = cache.entryFor(candidate.text, font,
                                                            style.label_halo_width, haloColour,
                                                            textColour, ratio);

            // Snapped to whole pixels. Blitting at a fractional position makes
            // Qt resample, and resampled text is visibly soft; half a pixel of
            // placement error on a place name is not.
            const QPointF where = box.topLeft() + entry.offset;
            painter.drawImage(QPointF(std::round(where.x()), std::round(where.y())), entry.image);
            ++stats.placed;
            continue;
        }

        // ---- a line label: the name laid along its road, perhaps more than
        // once. Where it lands and what it claims are both decided by the run
        // rather than by a box around the anchor.
        const RunWalker walker(runs.data() + candidate.runBegin, candidate.runCount);
        placeRepeatsAlongRun(candidate.text, walker, cache, font, emSize, repeatPx, placements);
        if (placements.empty())
        {
            // Too long for its road, or the road kinks under it. Either way
            // this name does not belong here -- and it may still be placed
            // from another tile, where more of the road is whole.
            ++stats.suppressed;
            continue;
        }

        for (const PlacedLabel& along : placements)
        {
            if (candidate.oneLabelPerName && claimedNearby(candidate.text, along.centre))
            {
                continue;
            }

            boxes.clear();
            if (along.uniformAngle.has_value())
            {
                // One box for a straight name, turned the way the name is.
                // Tighter than a per-character chain would be and cheaper to
                // test.
                QTransform turn;
                turn.translate(along.centre.x(), along.centre.y());
                turn.rotateRadians(*along.uniformAngle);
                boxes.push_back(turn.mapRect(QRectF(-text.width() / 2.0, -text.height() / 2.0,
                                                    text.width(), text.height())));
            }
            else
            {
                collisionBoxesFor(along.glyphs, text.height(), boxes);
            }
            pad(boxes);

            if (collides(boxes))
            {
                ++stats.suppressed;
                continue;
            }
            taken.insert(taken.end(), boxes.begin(), boxes.end());
            if (candidate.oneLabelPerName)
            {
                namesPlaced[candidate.text].push_back(along.centre);
            }

            // Only NOW is it worth pixels.
            if (!along.uniformAngle.has_value())
            {
                drawPlacedGlyphs(painter, along.glyphs, cache, font, style.label_halo_width,
                                 haloColour, textColour, ratio, baselineShift, glyphScratch);
                ++stats.placed;
                continue;
            }

            const LabelCache::Entry& entry = cache.entryFor(candidate.text, font,
                                                            style.label_halo_width, haloColour,
                                                            textColour, ratio);

            // A straight road's name, turned to lie along it: one blit, from
            // the same cached image a place name uses. A road running dead
            // flat across the screen takes the snapped path instead, which is
            // what keeps the commonest case in a grid city exactly as cheap
            // and as sharp as it was before any of this followed a road.
            if (std::abs(*along.uniformAngle) > 1e-6)
            {
                painter.save();
                painter.translate(along.centre);
                painter.rotate(*along.uniformAngle * 180.0 / std::numbers::pi);
                painter.drawImage(QPointF((-entry.bounds.width() / 2.0) + entry.offset.x(),
                                          (-entry.bounds.height() / 2.0) + entry.offset.y()),
                                  entry.image);
                painter.restore();
            }
            else
            {
                const QPointF topLeft(along.centre.x() - (entry.bounds.width() / 2.0),
                                      along.centre.y() - (entry.bounds.height() / 2.0));
                const QPointF where = topLeft + entry.offset;
                painter.drawImage(QPointF(std::round(where.x()), std::round(where.y())),
                                  entry.image);
            }
            ++stats.placed;
        }
    }

    return stats;
}

} // namespace map_widget
