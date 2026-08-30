// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_controls/view_mode_button.h"

#include <QPainter>
#include <QPen>

namespace map_controls
{

ViewModeButton::ViewModeButton(const QColor& glyph, const QColor& disc, QWidget* parent) :
    MapButton(glyph, disc, parent)
{
    setObjectName(QStringLiteral("mapViewModeButton"));
    setPerspective(false);
    // setPerspective early-outs on no change, so the first tooltip is set
    // here where the initial state is being established.
    setToolTip(QStringLiteral("Tilt the map into perspective"));
}

void ViewModeButton::setPerspective(bool perspective)
{
    if (perspective == mPerspective)
    {
        return;
    }
    mPerspective = perspective;
    setToolTip(mPerspective ? QStringLiteral("Look straight down")
                            : QStringLiteral("Tilt the map into perspective"));
    update();
}

void ViewModeButton::paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph)
{
    // The view a press WOULD GIVE -- see the header. Both marks are the same
    // ground square: seen face-on, and leaned back into a trapezoid.
    const QPointF centre = box.center();
    const double half = box.width() * 0.24;

    QPen pen(glyph);
    pen.setWidthF(1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPolygonF mark;
    if (mPerspective)
    {
        // Flat: press to look straight down.
        mark << QPointF(centre.x() - half, centre.y() - half)
             << QPointF(centre.x() + half, centre.y() - half)
             << QPointF(centre.x() + half, centre.y() + half)
             << QPointF(centre.x() - half, centre.y() + half);
    }
    else
    {
        // Leaning back: press for perspective. The top edge is shorter and
        // the whole shape squatter, which is exactly what the map does.
        const double top = half * 0.55;
        const double squash = half * 0.72;
        mark << QPointF(centre.x() - top, centre.y() - squash)
             << QPointF(centre.x() + top, centre.y() - squash)
             << QPointF(centre.x() + half, centre.y() + squash)
             << QPointF(centre.x() - half, centre.y() + squash);
    }
    painter.drawPolygon(mark);
}

} // namespace map_controls
