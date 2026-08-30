// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_controls/compass_button.h"

#include <QPainter>
#include <QPen>

#include <cmath>

namespace map_controls
{

CompassButton::CompassButton(const QColor& glyph, const QColor& disc, QWidget* parent) :
    MapButton(glyph, disc, parent)
{
    setToolTip(QStringLiteral("Switch between north-up and heading-up"));
    setObjectName(QStringLiteral("mapCompassButton"));
}

void CompassButton::setBearing(double degrees)
{
    if (degrees == mBearing)
    {
        return;
    }
    mBearing = degrees;
    // Quiet when it has nothing to say: needle straight up IS north-up on
    // screen, whichever mode produced it. Never hidden -- this button is the
    // only way into heading-up; see the header.
    setEmphasis(std::abs(std::remainder(mBearing, 360.0)) < 0.5 ? kDeEmphasized : 1.0);
    update();
}

void CompassButton::paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph)
{
    // A needle pointing at north ON SCREEN: two kite halves about the centre,
    // the north half filled solid, the south half only outlined -- the shape
    // every compass rose reduces to when it has to survive 20 pixels.
    const QPointF centre = box.center();
    const double reach = box.width() * 0.30;
    const double waist = box.width() * 0.13;

    painter.save();
    painter.translate(centre);
    painter.rotate(-mBearing);

    QPolygonF north;
    north << QPointF(0.0, -reach) << QPointF(waist, 0.0) << QPointF(-waist, 0.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(glyph);
    painter.drawPolygon(north);

    QPolygonF south;
    south << QPointF(0.0, reach) << QPointF(waist, 0.0) << QPointF(-waist, 0.0);
    QPen pen(glyph);
    pen.setWidthF(1.2);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(south);

    painter.restore();
}

} // namespace map_controls
