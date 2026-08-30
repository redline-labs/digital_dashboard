// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_controls/zoom_button.h"

#include <QPainter>
#include <QPen>

namespace map_controls
{

ZoomButton::ZoomButton(Direction direction, const QColor& glyph, const QColor& disc,
                       QWidget* parent) :
    MapButton(glyph, disc, parent), mDirection(direction)
{
    const bool in = direction == Direction::In;
    setToolTip(in ? QStringLiteral("Zoom in") : QStringLiteral("Zoom out"));
    setObjectName(in ? QStringLiteral("mapZoomInButton") : QStringLiteral("mapZoomOutButton"));

    // Hold to keep zooming. The delay matches a wheel's feel: one step on the
    // press, then a steady stream once it is clearly a hold.
    setAutoRepeat(true);
    setAutoRepeatDelay(400);
    setAutoRepeatInterval(120);
}

void ZoomButton::paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph)
{
    const QPointF centre = box.center();
    const double arm = box.width() * 0.18;

    QPen pen(glyph);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    painter.drawLine(QPointF(centre.x() - arm, centre.y()), QPointF(centre.x() + arm, centre.y()));
    if (mDirection == Direction::In)
    {
        painter.drawLine(QPointF(centre.x(), centre.y() - arm),
                         QPointF(centre.x(), centre.y() + arm));
    }
}

} // namespace map_controls
