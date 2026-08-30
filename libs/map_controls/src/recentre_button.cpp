// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_controls/recentre_button.h"

#include <QPainter>
#include <QPen>

namespace map_controls
{

RecentreButton::RecentreButton(const QColor& glyph, const QColor& disc, QWidget* parent) :
    MapButton(glyph, disc, parent)
{
    setToolTip(QStringLiteral("Recentre the map"));
    setObjectName(QStringLiteral("mapRecentreButton"));
}

void RecentreButton::paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph)
{
    // A locate glyph: a ring, a centre dot, and four ticks on the axes. It
    // reads as "put me back in the middle" without a word of text, which
    // matters because there is no room for one and no translation pipeline.
    const QPointF centre = box.center();

    QPen pen(glyph);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const double ring = box.width() * 0.26;
    painter.drawEllipse(centre, ring, ring);

    const double tickFrom = ring * 1.35;
    const double tickTo = ring * 1.95;
    painter.drawLine(QPointF(centre.x(), centre.y() - tickFrom),
                     QPointF(centre.x(), centre.y() - tickTo));
    painter.drawLine(QPointF(centre.x(), centre.y() + tickFrom),
                     QPointF(centre.x(), centre.y() + tickTo));
    painter.drawLine(QPointF(centre.x() - tickFrom, centre.y()),
                     QPointF(centre.x() - tickTo, centre.y()));
    painter.drawLine(QPointF(centre.x() + tickFrom, centre.y()),
                     QPointF(centre.x() + tickTo, centre.y()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(glyph);
    painter.drawEllipse(centre, ring * 0.34, ring * 0.34);
}

} // namespace map_controls
