#ifndef DASHBOARD_GAUGE_PAINTING_H_
#define DASHBOARD_GAUGE_PAINTING_H_

#include <QColor>
#include <QPainter>
#include <QPolygonF>
#include <QWidget>

#include <algorithm>

namespace gauge_paint {

// Shared needle/pivot styling across the analog gauges.
inline constexpr QColor kNeedleColor(255, 165, 0);
inline constexpr QColor kPivotColor(40, 40, 40);

// Centers the painter on the widget and scales uniformly so drawing code can
// work on a fixed logical canvas (default 200x200, i.e. radius 100).
inline void applyCenteredScale(QPainter& painter, const QWidget& widget, float logical_size = 200.0f)
{
    const int side = std::min(widget.width(), widget.height());
    painter.translate(widget.width() / 2.0f, widget.height() / 2.0f);
    painter.scale(side / logical_size, side / logical_size);
}

// Maps a value in [min, max] onto a dial angle: start_deg + proportion * sweep_deg.
inline float valueToAngleDeg(float value, float min, float max, float start_deg, float sweep_deg)
{
    if (max <= min)
    {
        return start_deg;
    }
    const float proportion = (std::clamp(value, min, max) - min) / (max - min);
    return start_deg + proportion * sweep_deg;
}

// Draws a tapered needle from the painter origin along +x at the current
// rotation; callers rotate the painter to the desired angle first.
inline void drawTaperedNeedle(QPainter& painter, float length, float base_width, float tip_width,
                              QColor color = kNeedleColor)
{
    QPolygonF needle;
    needle << QPointF(0.0f, -base_width / 2.0f)
           << QPointF(length, -tip_width / 2.0f)
           << QPointF(length, tip_width / 2.0f)
           << QPointF(0.0f, base_width / 2.0f);
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(needle);
    painter.restore();
}

// Draws the needle pivot dot at the painter origin.
inline void drawPivot(QPainter& painter, float radius, QColor color = kPivotColor)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(0.0f, 0.0f), radius, radius);
    painter.restore();
}

// Draws the circular gauge face centered at the painter origin.
inline void drawCircularBackground(QPainter& painter, float radius = 100.0f, QColor color = Qt::black)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(0.0f, 0.0f), radius, radius);
    painter.restore();
}

}  // namespace gauge_paint

#endif  // DASHBOARD_GAUGE_PAINTING_H_
