#include "motec_c125_tachometer/motec_c125_tachometer.h"

#include <QPainter>

#include <cmath>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "dashboard/gauge_painting.h"
#include "dashboard/widget_fonts.h"
#include "helpers/unit_conversion.h"

namespace
{
// Measurement sweep (ticks/labels/yellow value) — narrower than the arc span
constexpr float kSweepStartDeg = 165.0f; // start of the RPM scale (0 RPM)
constexpr float kSweepEndDeg   = 15.0f;  // end of the RPM scale (max RPM)
constexpr float kSweepTotalDeg = (kSweepEndDeg < kSweepStartDeg)
                                 ? (360.0f - (kSweepStartDeg - kSweepEndDeg))
                                 : (kSweepEndDeg - kSweepStartDeg);

// Visual arc span (outer/inner rings and base fill) — intentionally wider than the sweep
constexpr float kArcStartDeg   = 140.0f; // arc visuals begin earlier
constexpr float kArcEndDeg     = 40.0f;  // and end later than the sweep
constexpr float kArcTotalDeg   = (kArcEndDeg < kArcStartDeg)
                                 ? (360.0f - (kArcStartDeg - kArcEndDeg))
                                 : (kArcEndDeg - kArcStartDeg);

// Yellow value arc starts at the visual arc start, but the scale (0 RPM) begins later.
// This offset ensures a small yellow segment is visible before zero.
constexpr float kValueStartOffsetDeg = (kSweepStartDeg >= kArcStartDeg)
                                       ? (kSweepStartDeg - kArcStartDeg)
                                       : (kSweepStartDeg + 360.0f - kArcStartDeg);

// Dial geometry and styling (all angles map across elements)
// Outer dial arc (center-line radius and pen width)
constexpr float kOuterRingRadius = 90.0f;    // controls distance of the outer gray arc from center
constexpr float kOuterRingPen    = 6.0f;     // stroke width for outer arc
constexpr QColor kOuterRingColor = QColor(200, 200, 200);      // grayscale color (R=G=B)

// Inner dial arc (center-line radius and pen width)
constexpr float kInnerRingRadius = 60.0f;    // controls distance of the inner gray arc from center
constexpr float kInnerRingPen    = 6.0f;    // stroke width for inner arc
constexpr QColor kInnerRingColor = QColor(200, 200, 200);      // grayscale color (R=G=B)

// Channel (gap) derived from arcs; used for ticks, labels and fill
constexpr float kChannelInnerEdge   = kInnerRingRadius + (kInnerRingPen / 2.0f); // outer edge of inner arc
constexpr float kChannelOuterEdge   = kOuterRingRadius - (kOuterRingPen / 2.0f); // inner edge of outer arc
constexpr float kChannelThickness   = kChannelOuterEdge - kChannelInnerEdge;     // width of the gap between arcs
constexpr float kChannelCenterRad   = (kChannelOuterEdge + kChannelInnerEdge) * 0.5f; // center radius of the gap

// Tick layout (all ticks are drawn within the channel)
constexpr float kTickOuterRadius    = kChannelOuterEdge - 0.5f; // small margin from the outer arc
constexpr float kTickInnerMajor     = kTickOuterRadius - 6.5f;  // major tick length
constexpr float kTickInnerMid       = kTickOuterRadius - 4.0f;  // mid tick length
constexpr float kTickInnerMinor     = kTickOuterRadius - 2.5f;  // minor tick length
constexpr float kTickWidthMajor     = 2.4f;                     // major tick stroke width
constexpr float kTickWidthMid       = 1.8f;                     // mid tick stroke width
constexpr float kTickWidthMinor     = 1.0f;                     // minor tick stroke width

// Label layout
constexpr float kLabelRadius        = kTickInnerMajor - 8.0f;   // place labels closer to the inner arc
constexpr float kLabelFontSize      = 12.0f;                    // scale relative to center digit font

// Fill band styling
constexpr float kFillArcRadius      = kChannelCenterRad;        // centered in the channel
constexpr float kFillArcThickness   = kChannelThickness;        // completely fills the channel (no gap)

constexpr QColor kFillBaseColor    = QColor(255, 255, 255);     // white background of the fill
constexpr QColor kFillValueColor   = QColor(255, 180, 0);       // yellow overlay color
}

MotecC125Tachometer::MotecC125Tachometer(const MotecC125TachometerConfig_t& cfg, QWidget* parent):
  dashboard::CachedPaintWidget(parent),
  _cfg{cfg},
  _rpm{0.0f}
{
    // font for the center digit (use bundled Futura if available)
    _digitFont = QFont(dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica"), 40, QFont::Bold);

    _expression_parser = dashboard::makeExpressionSubscription<float>(
        _cfg.schema_type, _cfg.rpm_expression, _cfg.zenoh_key,
        this, &MotecC125Tachometer::setRpm, "C125 tachometer rpm");
}

void MotecC125Tachometer::setRpm(float rpm)
{
    _rpm = std::clamp(rpm, 0.0f, static_cast<float>(_cfg.max_rpm));
    update();
}

void MotecC125Tachometer::applyPaintTransform(QPainter& painter) const
{
    gauge_paint::applyCenteredScale(painter, *this); // normalize to 200x200 box
}

void MotecC125Tachometer::paintStaticUnderlay(QPainter& painter)
{
    // Solid background shape under everything (keeps bottom truncated)
    drawBackdrop(&painter);
    // Full-span white fill band; the yellow value arc is drawn over it per frame
    drawBaseArc(&painter);
}

void MotecC125Tachometer::paintDynamic(QPainter& painter)
{
    drawValueArc(&painter);
}

void MotecC125Tachometer::paintStaticOverlay(QPainter& painter)
{
    // Outlines and ticks sit on top of the fill band
    drawDial(&painter);
    drawTicks(&painter);
    drawCenterDigit(&painter);
    drawRedline(&painter);
}

void MotecC125Tachometer::drawRedline(QPainter* painter)
{
    // Redline overlay along outer arc (thin red stroke from configured RPM to end of arc)
    if (_cfg.redline_rpm == 0 || _cfg.redline_rpm > _cfg.max_rpm)
    {
        return;
    }

    painter->save();
    // Map redline RPM into sweep space, then convert to absolute angle along the visual arc
    const float proportion = std::clamp(static_cast<float>(_cfg.redline_rpm) / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    const float sweep_at_red = kSweepTotalDeg * proportion; // degrees after sweep start
    const float redline_start_deg = kValueStartOffsetDeg + sweep_at_red; // relative to visual arc start

    QPen redPen(QColor(220, 0, 0));
    redPen.setWidthF(2.0f);
    redPen.setCapStyle(Qt::FlatCap);
    painter->setPen(redPen);

    const QRectF outerRect(-kOuterRingRadius, -kOuterRingRadius, 2*kOuterRingRadius, 2*kOuterRingRadius);
    // Start from arc start + redline angle, draw to the visual arc end clockwise (negative span)
    const int start_qt = static_cast<int>(-(kArcStartDeg + redline_start_deg) * 16.0f);
    const int span_qt  = static_cast<int>(-(kArcTotalDeg - redline_start_deg) * 16.0f);
    painter->drawArc(outerRect, start_qt, span_qt);
    painter->restore();
}

void MotecC125Tachometer::drawDial(QPainter* painter)
{
    painter->save();

    // No background here (handled in drawBackdrop)
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::NoBrush);

    // Arc visuals use the wider visual span
    constexpr int start_qt = static_cast<int>(-kArcStartDeg * 16.0f);
    constexpr int span_qt  = static_cast<int>(-kArcTotalDeg * 16.0f);

    // Outer ring (medium/light gray)
    QPen outerPen(kOuterRingColor);
    outerPen.setWidthF(kOuterRingPen);
    outerPen.setCapStyle(Qt::FlatCap);
    painter->setPen(outerPen);
    painter->setBrush(Qt::NoBrush);
    QRectF outerRect(-kOuterRingRadius, -kOuterRingRadius, 2*kOuterRingRadius, 2*kOuterRingRadius);
    painter->drawArc(outerRect, start_qt, span_qt);

    // Inner ring (medium/light gray)
    QPen innerPen(kInnerRingColor);
    innerPen.setWidthF(kInnerRingPen);
    innerPen.setCapStyle(Qt::FlatCap);
    painter->setPen(innerPen);
    QRectF innerRect(-kInnerRingRadius, -kInnerRingRadius, 2*kInnerRingRadius, 2*kInnerRingRadius);
    painter->drawArc(innerRect, start_qt, span_qt);

    painter->restore();
}

void MotecC125Tachometer::drawBackdrop(QPainter* painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawRoundedRect(QRectF(-90, -70, 180, 140), 8, 8);
    painter->restore();
}

void MotecC125Tachometer::drawTicks(QPainter* painter)
{
    painter->save();

    const QColor tickColor = Qt::black;
    QPen pen(tickColor);
    pen.setCapStyle(Qt::FlatCap);
    painter->setPen(pen);

    // Draw ticks at RPM-based intervals (100, 500, 1000)
    for (uint32_t rpm = 0; rpm <= _cfg.max_rpm; rpm += 100)
    {
        float proportion = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        float a = kSweepStartDeg + kSweepTotalDeg * proportion; // clockwise
        if (a >= 360.0f) a -= 360.0f;
        float rad = degrees_to_radians(a);

        bool isMajor = (rpm % 1000 == 0);
        bool isMid   = (!isMajor && (rpm % 500 == 0));
        pen.setWidthF(isMajor ? kTickWidthMajor : (isMid ? kTickWidthMid : kTickWidthMinor));
        painter->setPen(pen);

        float inner = isMajor ? kTickInnerMajor : (isMid ? kTickInnerMid : kTickInnerMinor);
        QPointF p1(inner * std::cos(rad), inner * std::sin(rad));
        QPointF p2(kTickOuterRadius * std::cos(rad), kTickOuterRadius * std::sin(rad));
        painter->drawLine(p1, p2);
    }

    // Labels for each 1000 RPM (single digit 1..)
    QFont labelFont = _digitFont;
    labelFont.setPointSizeF(kLabelFontSize);
    painter->setFont(labelFont);
    painter->setPen(Qt::black);
    // Place labels closer to inner arc to avoid overlap with shorter ticks
    constexpr float label_radius = kLabelRadius;
    for (uint32_t rpm = 0u; rpm <= _cfg.max_rpm; rpm += 1000u)
    {
        float proportion = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        float a = kSweepStartDeg + kSweepTotalDeg * proportion;
        if (a >= 360.0f) a -= 360.0f;
        float rad = degrees_to_radians(a);
        QString text = QString::number(static_cast<int>(rpm / 1000));
        QRectF r(0,0,16,12);
        r.moveCenter(QPointF(label_radius * std::cos(rad), label_radius * std::sin(rad)));
        painter->drawText(r, Qt::AlignCenter, text);
    }

    painter->restore();
}

void MotecC125Tachometer::drawBaseArc(QPainter* painter)
{
    painter->save();

    // Match the widened gap between inner and outer rings so the fill overlays that channel with no gap
    constexpr QRectF rect(-kFillArcRadius, -kFillArcRadius, 2 * kFillArcRadius, 2 * kFillArcRadius);

    // The entire visual arc span in white as the background so it matches the gray rings
    QPen bgPen(kFillBaseColor);
    bgPen.setWidthF(kFillArcThickness);
    bgPen.setCapStyle(Qt::FlatCap);
    painter->setPen(bgPen);
    painter->drawArc(rect, static_cast<int>(-kArcStartDeg * 16.0f), static_cast<int>(-kArcTotalDeg * 16.0f));

    painter->restore();
}

void MotecC125Tachometer::drawValueArc(QPainter* painter)
{
    painter->save();

    // Compute proportion and value sweep within the inset sweep range
    const float proportion = std::clamp(static_cast<float>(_rpm) / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    const float sweep_deg = kSweepTotalDeg * proportion;

    constexpr QRectF rect(-kFillArcRadius, -kFillArcRadius, 2 * kFillArcRadius, 2 * kFillArcRadius);

    // Overlay the current portion in yellow; start from the visual arc start so
    // a small pre-zero yellow segment is visible up to the 0 tick
    QPen valPen(kFillValueColor);
    valPen.setWidthF(kFillArcThickness);
    valPen.setCapStyle(Qt::FlatCap);
    painter->setPen(valPen);
    // total sweep for the yellow arc = offset to zero + value proportion of sweep
    const float yellow_total_deg = kValueStartOffsetDeg + sweep_deg;
    painter->drawArc(rect, static_cast<int>(-kArcStartDeg * 16.0f), static_cast<int>(-yellow_total_deg * 16.0f));

    painter->restore();
}

void MotecC125Tachometer::drawCenterDigit(QPainter* painter)
{
    constexpr QColor digit_color = QColor(255, 255, 255);
    constexpr QColor rpm_label_color = QColor(100, 100, 100);

    painter->save();
    painter->setPen(digit_color);
    painter->setFont(_digitFont);

    const QString digit = QString::number(static_cast<int>(_cfg.center_page_digit));
    QRectF r(-40, -35, 80, 70);
    painter->drawText(r, Qt::AlignCenter, digit);

    // Small label RPMx1000 at bottom of center
    QFont small = _digitFont;
    small.setPointSizeF(8.0f);
    painter->setFont(small);
    painter->setPen(rpm_label_color);
    painter->drawText(QRectF(-40, 15, 80, 20), Qt::AlignCenter, "RPMx1000");
    painter->restore();
}

#include "motec_c125_tachometer/moc_motec_c125_tachometer.cpp"


