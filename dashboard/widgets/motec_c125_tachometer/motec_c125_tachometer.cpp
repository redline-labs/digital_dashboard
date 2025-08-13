#include "motec_c125_tachometer/motec_c125_tachometer.h"

#include <QPainter>
#include <QFontDatabase>
#include <QMetaObject>

#include <cmath>
#include <numbers>

#include <spdlog/spdlog.h>

#include "expression_parser/expression_parser.h"

namespace {
constexpr float degToRad(float deg) { return deg * (std::numbers::pi_v<float> / 180.0f); }
// Keep sweep angles consistent across all elements
constexpr float kSweepStartDeg = 160.0f; // 0 RPM
constexpr float kSweepEndDeg   = 20.0f; // max RPM
}

MotecC125Tachometer::MotecC125Tachometer(const MotecC125TachometerConfig_t& cfg, QWidget* parent)
    : QWidget(parent), _cfg{cfg}, _rpm{0.0f} {
    // font for the center digit (use bundled Futura if available)
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
    if (fontId == -1) {
        _digitFont = QFont("Helvetica", 40, QFont::Bold);
    } else {
        const QString family = QFontDatabase::applicationFontFamilies(fontId).at(0);
        _digitFont = QFont(family, 40, QFont::Bold);
    }

    // Optional expression parser hookup
    try {
        _expression_parser = std::make_unique<expression_parser::ExpressionParser>(
            _cfg.schema_type, _cfg.rpm_expression, _cfg.zenoh_key);
        if (_expression_parser->isValid()) {
            _expression_parser->setResultCallback<float>([this](float rpm) {
                QMetaObject::invokeMethod(this, "onRpmEvaluated", Qt::QueuedConnection, Q_ARG(float, rpm));
            });
        } else {
            _expression_parser.reset();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("CircleTachometer: expression parser init failed: {}", e.what());
        _expression_parser.reset();
    }
}

void MotecC125Tachometer::setZenohSession(std::shared_ptr<zenoh::Session> session) {
    _zenoh_session = std::move(session);
}

float MotecC125Tachometer::clampRpm(float rpm) const {
    if (rpm < 0.0f) return 0.0f;
    if (rpm > static_cast<float>(_cfg.max_rpm)) return static_cast<float>(_cfg.max_rpm);
    return rpm;
}

void MotecC125Tachometer::setRpm(float rpm) {
    _rpm = clampRpm(rpm);
    update();
}

void MotecC125Tachometer::onRpmEvaluated(float rpm) {
    setRpm(rpm);
}

void MotecC125Tachometer::paintEvent(QPaintEvent* e) {
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int side = qMin(width(), height());
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(side / 200.0, side / 200.0); // normalize to 200x200 box

    // Solid background shape under everything (keeps bottom truncated)
    drawBackdrop(&p);
    // Draw the fill band first so it sits beneath outlines and ticks
    drawFilledArc(&p);
    // Then outlines and ticks on top
    drawDial(&p);
    drawTicks(&p);
    drawCenterDigit(&p);
}

void MotecC125Tachometer::drawDial(QPainter* painter) {
    painter->save();

    // No background here (handled in drawBackdrop)
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::NoBrush);

    // Use same sweep as the gauge for both outer/inner outlines so bottoms are truncated
    const float start_deg = kSweepStartDeg;
    const float end_deg   = kSweepEndDeg;
    const float total_sweep = (end_deg < start_deg) ? (360.0f - (start_deg - end_deg)) : (end_deg - start_deg);
    const int start_qt = static_cast<int>(-start_deg * 16.0f);
    const int span_qt  = static_cast<int>(-total_sweep * 16.0f);

    // Outer ring (light gray)
    QPen outerPen(QColor(220,220,220));
    outerPen.setWidthF(6.0f);
    outerPen.setCapStyle(Qt::FlatCap);
    painter->setPen(outerPen);
    painter->setBrush(Qt::NoBrush);
    QRectF outerRect(-80.0, -80.0, 160.0, 160.0);
    painter->drawArc(outerRect, start_qt, span_qt);

    // Inner ring (white)
    QPen innerPen(QColor(255,255,255));
    innerPen.setWidthF(10.0f);
    innerPen.setCapStyle(Qt::FlatCap);
    painter->setPen(innerPen);
    QRectF innerRect(-62.0, -62.0, 124.0, 124.0);
    painter->drawArc(innerRect, start_qt, span_qt);

    painter->restore();
}

void MotecC125Tachometer::drawBackdrop(QPainter* painter) {
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawRoundedRect(QRectF(-90, -70, 180, 140), 8, 8);
    painter->restore();
}

void MotecC125Tachometer::drawTicks(QPainter* painter) {
    painter->save();

    // Tick band lies in the dark area between the white rings
    // Inner white ring radius = 62 with pen 10 -> outer edge ~67
    // Place ticks just outside that, ending just inside the outer ring (80 with pen 6 -> inner edge ~77)
    const float tick_outer = 76.0f;           // tick line outer radius
    const float tick_inner_major = 68.5f;     // 1000 RPM
    const float tick_inner_mid = 70.5f;       // 500 RPM
    const float tick_inner_minor = 72.5f;     // 100 RPM

    const QColor tickColor = Qt::white;
    QPen pen(tickColor);
    pen.setCapStyle(Qt::FlatCap);
    painter->setPen(pen);

    // Sweep and mapping are the same as the yellow arc
    const float start_deg = kSweepStartDeg;
    const float end_deg = kSweepEndDeg;
    const float sweep_cw = (end_deg < start_deg) ? (end_deg + 360.0f - start_deg) : (end_deg - start_deg);

    // Draw ticks at RPM-based intervals (100, 500, 1000)
    for (uint32_t rpm = 0; rpm <= _cfg.max_rpm; rpm += 100) {
        float proportion = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        float a = start_deg + sweep_cw * proportion; // clockwise
        if (a >= 360.0f) a -= 360.0f;
        float rad = degToRad(a);

        bool isMajor = (rpm % 1000 == 0);
        bool isMid   = (!isMajor && (rpm % 500 == 0));
        pen.setWidthF(isMajor ? 3.6f : (isMid ? 2.4f : 1.4f));
        painter->setPen(pen);

        float inner = isMajor ? tick_inner_major : (isMid ? tick_inner_mid : tick_inner_minor);
        QPointF p1(inner * std::cos(rad), inner * std::sin(rad));
        QPointF p2(tick_outer * std::cos(rad), tick_outer * std::sin(rad));
        painter->drawLine(p1, p2);
    }

    // Labels for each 1000 RPM (single digit 1..)
    QFont labelFont = _digitFont;
    labelFont.setPointSizeF(std::max(8.0, labelFont.pointSizeF() * 0.28));
    painter->setFont(labelFont);
    painter->setPen(Qt::white);
    const float label_radius = 56.0f; // just inside inner ring
    for (uint32_t rpm = 1000; rpm <= _cfg.max_rpm; rpm += 1000) {
        float proportion = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        float a = start_deg + sweep_cw * proportion;
        if (a >= 360.0f) a -= 360.0f;
        float rad = degToRad(a);
        QString text = QString::number(static_cast<int>(rpm / 1000));
        QRectF r(0,0,16,12);
        r.moveCenter(QPointF(label_radius * std::cos(rad), label_radius * std::sin(rad)));
        painter->drawText(r, Qt::AlignCenter, text);
    }

    painter->restore();
}

void MotecC125Tachometer::drawFilledArc(QPainter* painter) {
    painter->save();

    // Use the same sweep as ticks for consistency
    const float start_deg = kSweepStartDeg; // where 0 rpm begins
    const float end_deg = kSweepEndDeg;     // where max rpm ends
    const float total_sweep = (end_deg < start_deg) ? (360.0f - (start_deg - end_deg)) : (end_deg - start_deg);

    const float proportion = std::clamp(static_cast<float>(_rpm) / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    const float sweep_deg = total_sweep * proportion;

    // Match the gap between inner and outer rings so the fill overlays exactly in that channel
    const float arc_radius = 72.0f;    // mid-point between ~67.5 and ~76
    const float arc_thickness = 10.0f; // roughly the gap width
    QRectF rect(-arc_radius, -arc_radius, 2*arc_radius, 2*arc_radius);

    // First draw the entire sweep in white as the background
    QPen bgPen(Qt::white);
    bgPen.setWidthF(arc_thickness);
    bgPen.setCapStyle(Qt::FlatCap);
    painter->setPen(bgPen);
    painter->drawArc(rect, static_cast<int>(-start_deg * 16.0f), static_cast<int>(-total_sweep * 16.0f));

    // Then overlay the current portion in yellow
    QPen valPen(QColor(255, 180, 0));
    valPen.setWidthF(arc_thickness);
    valPen.setCapStyle(Qt::FlatCap);
    painter->setPen(valPen);
    painter->drawArc(rect, static_cast<int>(-start_deg * 16.0f), static_cast<int>(-sweep_deg * 16.0f));

    painter->restore();
}

void MotecC125Tachometer::drawCenterDigit(QPainter* painter) {
    painter->save();
    painter->setPen(Qt::white);
    painter->setFont(_digitFont);

    const QString digit = QString::number(static_cast<int>(_cfg.center_page_digit));
    QRectF r(-40, -35, 80, 70);
    painter->drawText(r, Qt::AlignCenter, digit);

    // Small label RPMx1000 at bottom of center
    QFont small = _digitFont;
    small.setPointSizeF(std::max(8.0, small.pointSizeF() * 0.35));
    painter->setFont(small);
    painter->drawText(QRectF(-40, 15, 80, 20), Qt::AlignCenter, "RPMx1000");
    painter->restore();
}

#include "motec_c125_tachometer/moc_motec_c125_tachometer.cpp"


