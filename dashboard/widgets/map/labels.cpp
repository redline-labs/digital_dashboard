// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/labels.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QString>

#include <algorithm>
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

LabelStats paintLabels(QPainter& painter, const Projection& projection,
                       const std::vector<LabelTile>& tiles, const MapStyle_t& style)
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
        const mvt::Layer* layer = entry.tile->layer("place");
        if (layer == nullptr || layer->extent == 0)
        {
            continue;
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

            // name:latin, not name. This archive's tilemaker config emits only
            // the latin field, and reading `name` returns an empty string for
            // every place -- a map with no labels and no error anywhere.
            const std::string text = layer->attributeText(feature, "name:latin");
            if (text.empty())
            {
                continue;
            }

            // The tile's own axes are rotated with the map even though the text
            // is not, so the anchor has to go through the same rotation the GPU
            // applies to the geometry.
            const mvt::Point& p = feature.rings.front().front();
            const double lx = double(p.x) * scale;
            const double ly = double(p.y) * scale;
            const double rx = (lx * cosB) - (ly * sinB);
            const double ry = (lx * sinB) + (ly * cosB);

            candidates.push_back(
                Candidate { QString::fromStdString(text), ScreenPoint { origin.x + rx, origin.y + ry },
                            placePriority(layer->attributeText(feature, "class")) });
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
    const QFontMetricsF metrics(font);

    const QColor textColour = toQColor(style.label_text);
    const QColor haloColour = toQColor(style.label_halo);

    // A linear scan against accepted boxes. A viewport holds tens of labels,
    // not thousands, so a grid would cost more to maintain than it saves.
    std::vector<QRectF> taken;
    const QRectF viewport(0.0, 0.0, projection.viewportWidth(), projection.viewportHeight());

    for (const Candidate& candidate : candidates)
    {
        const QRectF text = metrics.boundingRect(candidate.text);
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

        // Halo by stroking the glyph outlines rather than drawing the text
        // several times at offsets: one path, one stroke, and the halo is even
        // on every side. The offset trick leaves the corners thin.
        QPainterPath glyphs;
        glyphs.addText(box.left(), box.top() + metrics.ascent(), font, candidate.text);

        if (style.label_halo_width > 0.0)
        {
            QPen halo(haloColour);
            halo.setWidthF(style.label_halo_width);
            halo.setJoinStyle(Qt::RoundJoin);
            painter.setPen(halo);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(glyphs);
        }

        painter.setPen(Qt::NoPen);
        painter.fillPath(glyphs, textColour);

        ++stats.placed;
    }

    return stats;
}

} // namespace map_widget
