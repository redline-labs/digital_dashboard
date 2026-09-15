#ifndef DASHBOARD_GAUGE_PAINTING_H_
#define DASHBOARD_GAUGE_PAINTING_H_

#include <QColor>
#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

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

// Clamps a reading to a configured range, tolerating the two things a config
// file can hand you that std::clamp cannot take.
//
// std::clamp's precondition is !(max < min) -- an inverted range is undefined
// behaviour, and min/max come straight from YAML with nothing checking their
// order. And std::clamp passes NaN through unchanged, because both of its
// comparisons are false for NaN, so a NaN reading would sail past a "clamp" and
// into painter.rotate(). Every gauge setter should go through here.
inline float clampToRange(float value, float min, float max)
{
    if (max < min)
    {
        std::swap(min, max);
    }

    // Park at the bottom of the range rather than propagate a non-finite value.
    if (!std::isfinite(value))
    {
        return min;
    }

    return std::clamp(value, min, max);
}

// Maps a value in [min, max] onto a dial angle: start_deg + proportion * sweep_deg.
inline float valueToAngleDeg(float value, float min, float max, float start_deg, float sweep_deg)
{
    if (max <= min)
    {
        return start_deg;
    }
    const float proportion = (clampToRange(value, min, max) - min) / (max - min);
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

// Draws one radial tick at `angle_deg`, between two radii from the painter
// origin. The angle is in the same convention the gauges use -- degrees,
// measured the way valueToAngleDeg returns them -- and the negation Qt's
// downward y axis needs happens here rather than at each call site.
//
// This block ("angle -> cos/sin -> two points -> drawLine") was copy-pasted five
// times, twice inside mercedes_190e_speedometer.cpp alone, once per direction
// the ticks grow. r_from/r_to being independent is what covers both: ticks that
// grow outward pass the smaller radius first, ticks that grow inward pass the
// larger.
inline void drawRadialTick(QPainter& painter, float angle_deg, float r_from, float r_to)
{
    const float radians = -angle_deg * static_cast<float>(M_PI) / 180.0f;
    const float cos_a = std::cos(radians);
    const float sin_a = std::sin(radians);
    painter.drawLine(QPointF(r_from * cos_a, r_from * sin_a),
                     QPointF(r_to * cos_a, r_to * sin_a));
}

// Draws `text` centred on a point at `radius` and `angle_deg` from the origin.
// The companion to drawRadialTick: every gauge that draws ticks also labels
// them, with the same boundingRect/moveCenter/drawText dance each time.
inline void drawTextAtAngle(QPainter& painter, const QFontMetricsF& fm, float angle_deg,
                            float radius, const QString& text)
{
    const float radians = -angle_deg * static_cast<float>(M_PI) / 180.0f;
    QRectF box = fm.boundingRect(text);
    box.moveCenter(QPointF(radius * std::cos(radians), radius * std::sin(radians)));
    painter.drawText(box, Qt::AlignCenter, text);
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
