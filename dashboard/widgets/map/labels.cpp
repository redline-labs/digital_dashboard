// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/labels.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>

namespace map_widget
{
namespace
{

struct Candidate
{
    QString text;
    ScreenPoint at;
    int priority { 0 };
};

QColor toQColor(const helpers::Color& colour)
{
    return QColor(QString::fromStdString(colour.value()));
}

// Which source layers carry labels, and how each ranks its own.
//
// A TABLE rather than a hardcoded layer(), because there is more than one now:
// the basemap's `place` and the track archive's `track_label`, which arrive in
// the same viewport from two different archives. They compete for the same
// screen space and are placed by one pass, which is the whole reason the
// candidates are gathered before any of them is drawn.
struct LabelLayerSpec
{
    const char* sourceLayer;
    int (*priority)(const mvt::Layer&, const mvt::Feature&);
};

int placeLayerPriority(const mvt::Layer& layer, const mvt::Feature& feature)
{
    return placePriority(layer.attributeText(feature, "class"));
}

int trackLayerPriority(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // Between a town and a city. A circuit is a landmark worth seeing from a
    // distance, and it is also the reason the driver is looking at this part of
    // the map -- but it must not push a city name off a country view.
    (void)layer;
    (void)feature;
    return 2;
}

constexpr std::array<LabelLayerSpec, 2> kLabelLayers { {
    { "place", placeLayerPriority },
    { "track_label", trackLayerPriority },
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
                  const Projection& projection, std::vector<Candidate>& candidates)
{
    const mvt::Layer* layer = entry.tile->layer(spec.sourceLayer);
    if (layer == nullptr || layer->extent == 0)
    {
        return;
    }

    const ScreenPoint origin = projection.tileOrigin(entry.id);
    const double size = projection.tileScreenSize(entry.id.z);
    const double scale = size / double(layer->extent);
    const double bearing = projection.camera().bearing;
    const double radians = -bearing * 3.14159265358979323846 / 180.0;
    const double cosB = std::cos(radians);
    const double sinB = std::sin(radians);

    for (const mvt::Feature& feature : layer->features)
    {
        if (feature.type != mvt::GeomType::Point || feature.rings.empty() ||
            feature.rings.front().empty())
        {
            continue;
        }

        // name:latin, not name. This archive's tilemaker config emits only the
        // latin field, and reading `name` returns an empty string for every
        // place -- a map with no labels and no error anywhere. map_build writes
        // BOTH spellings for exactly this reason, so the track layer is safe
        // either way.
        const std::string text = layer->attributeText(feature, "name:latin");
        if (text.empty())
        {
            continue;
        }

        // The tile's own axes are rotated with the map even though the text is
        // not, so the anchor has to go through the same rotation the GPU
        // applies to the geometry.
        const mvt::Point& p = feature.rings.front().front();
        const double lx = double(p.x) * scale;
        const double ly = double(p.y) * scale;
        const double rx = (lx * cosB) - (ly * sinB);
        const double ry = (lx * sinB) + (ly * cosB);

        candidates.push_back(Candidate { QString::fromStdString(text),
                                         ScreenPoint { origin.x + rx, origin.y + ry },
                                         spec.priority(*layer, feature) });
    }
}

} // namespace

int placePriority(std::string_view className)
{
    if (className == "country")
    {
        return 5;
    }
    if (className == "state" || className == "province")
    {
        return 4;
    }
    if (className == "city")
    {
        return 3;
    }
    if (className == "town")
    {
        return 2;
    }
    if (className == "village" || className == "suburb" || className == "neighbourhood")
    {
        return 1;
    }
    return 0;
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
    }

    if (const auto found = mEntries.constFind(text); found != mEntries.constEnd())
    {
        return *found;
    }

    // A long drive through many named places would otherwise grow this without
    // bound. Dropping everything is fine: the visible labels are rebuilt over
    // the next few frames.
    constexpr int kMaxEntries = 512;
    if (mEntries.size() >= kMaxEntries)
    {
        mEntries.clear();
    }

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

    // Same geometry the old inline path used: the glyph origin sits at
    // (0, ascent) from the placement box's top left, which is `pad` in here.
    QPainterPath glyphs;
    glyphs.addText(pad, pad + metrics.ascent(), font, text);

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

    for (const LabelTile& entry : tiles)
    {
        if (!entry.tile)
        {
            continue;
        }
        for (const LabelLayerSpec& spec : kLabelLayers)
        {
            gatherLabels(entry, spec, projection, candidates);
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.priority > b.priority; });

    QFont font = painter.font();
    if (!style.label_font.empty())
    {
        font.setFamily(QString::fromStdString(style.label_font));
    }
    font.setPointSizeF(double(style.label_size));
    painter.setFont(font);
    const QColor textColour = toQColor(style.label_text);
    const QColor haloColour = toQColor(style.label_halo);

    // A linear scan against accepted boxes. A viewport holds tens of labels,
    // not thousands, so a grid would cost more to maintain than it saves.
    std::vector<QRectF> taken;
    const QRectF viewport(0.0, 0.0, projection.viewportWidth(), projection.viewportHeight());

    for (const Candidate& candidate : candidates)
    {
        // Rendered once per string, not once per string per frame -- and the
        // bounding rect comes back with it, so the placement pass does not
        // re-measure either.
        const LabelCache::Entry& entry =
            cache.entryFor(candidate.text, font, style.label_halo_width, haloColour, textColour,
                           painter.device() != nullptr ? painter.device()->devicePixelRatioF() : 1.0);
        const QRectF& text = entry.bounds;
        const QRectF box(candidate.at.x - (text.width() / 2.0),
                         candidate.at.y - (text.height() / 2.0), text.width(), text.height());

        if (!viewport.intersects(box))
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
