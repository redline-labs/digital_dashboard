#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"

#include <QPainter>
#include <QFontDatabase>
#include <QMetaObject>

#include <cmath>
#include <numbers>
#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>

#include "expression_parser/expression_parser.h"

namespace {
constexpr float degToRad(float deg) { return deg * (std::numbers::pi_v<float> / 180.0f); }

// Define the sweep angles following the MoTeC CDL3 top arc feel
// Start on the LEFT shoulder and sweep CLOCKWISE to the RIGHT shoulder
constexpr float kSweepStartDeg = 170.0f; // start at left shoulder (0 RPM)
constexpr float kSweepEndDeg   = 70.0f;  // end at right shoulder  (max RPM)

// Piecewise angle mapping to imitate a slightly non-linear scale seen on CDL3
// We'll bias low range to sweep faster (bigger angle per 1000) and compress near top.
}

MotecCdl3Tachometer::MotecCdl3Tachometer(const MotecCdl3TachometerConfig_t& cfg, QWidget* parent)
    : QWidget(parent), _cfg{cfg}, _rpm{0.0f} {
    // Load segmented display font (DSEG)
    int dsegId = QFontDatabase::addApplicationFont(":/fonts/DSEG7Classic-Bold.ttf");
    if (dsegId == -1) {
        _segmentFont = QFont("Helvetica", 10, QFont::Bold);
        SPDLOG_WARN("Failed to load DSEG font, falling back to system font");
    } else {
        const QString family = QFontDatabase::applicationFontFamilies(dsegId).at(0);
        _segmentFont = QFont(family, 10, QFont::Bold);
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
        SPDLOG_ERROR("MotecCdl3Tachometer: expression parser init failed: {}", e.what());
        _expression_parser.reset();
    }
}

void MotecCdl3Tachometer::setZenohSession(std::shared_ptr<zenoh::Session> session) {
    _zenoh_session = std::move(session);
}

void MotecCdl3Tachometer::setRpm(float rpm) {
    if (rpm < 0.0f) rpm = 0.0f;
    if (rpm > static_cast<float>(_cfg.max_rpm)) rpm = static_cast<float>(_cfg.max_rpm);
    _rpm = rpm;
    update();
}

void MotecCdl3Tachometer::onRpmEvaluated(float rpm) { setRpm(rpm); }

void MotecCdl3Tachometer::paintEvent(QPaintEvent* e) {
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int side = qMin(width(), height());
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(side / 220.0, side / 220.0); // normalize to 220x220 box, slightly larger

    drawSweepBands(&p);
    drawTicksAndLabels(&p);
}

// Non-linear mapping from RPM proportion to angle. We use a simple quadratic ease-out near top.
float MotecCdl3Tachometer::mapRpmToAngleDeg(float rpm) const {
    const float proportion = std::clamp(rpm / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    // ease: y = 1 - (1 - x)^2 -> fast at start, slow near end (matches perceived spacing)
    const float eased = 1.0f - (1.0f - proportion) * (1.0f - proportion);
    const float sweep_cw = (kSweepStartDeg >= kSweepEndDeg)
                               ? (kSweepStartDeg - kSweepEndDeg)
                               : (kSweepStartDeg + 360.0f - kSweepEndDeg);
    // Decrease angle with rpm so sweep proceeds CLOCKWISE from left to right
    return kSweepStartDeg - sweep_cw * eased;
}

// The visible band radius varies with angle to mimic the CDL3 taper (shallower near start, deeper near middle)
float MotecCdl3Tachometer::mapAngleToRadius(float angle_deg) const {
    // Normalize to 0..1 across the sweep (clockwise)
    const float sweep_cw = (kSweepEndDeg >= kSweepStartDeg) ? (kSweepEndDeg - kSweepStartDeg)
                                                           : (kSweepEndDeg + 360.0f - kSweepStartDeg);
    float pos = angle_deg - kSweepStartDeg;
    if (pos < 0.0f) pos += 360.0f;
    const float t = std::clamp(pos / sweep_cw, 0.0f, 1.0f);

    // Flatten at higher RPMs by increasing radius with t (non-constant radius path)
    // Start narrower (more curved), end wider (flatter)
    const float base_inner = 60.0f;
    const float base_outer = 86.0f; // target outer bound for the channel center

    // Use a smoothstep-like curve and a small mid bulge to avoid a perfectly circular feel
    const float grow = std::pow(t, 0.55f); // faster growth early, gentle near end
    const float bulge = 0.06f * std::sin(static_cast<float>(std::numbers::pi) * t);
    const float center = base_inner + (base_outer - base_inner) * (0.70f + 0.25f * grow + bulge);
    return center;
}

void MotecCdl3Tachometer::drawSweepBands(QPainter* painter) {
    painter->save();

    // Elliptical path to mimic CDL3: top arc flatter than sides
    const float start = kSweepStartDeg;
    const float end = kSweepEndDeg;
    const float sweep_cw = (start >= end) ? (start - end) : (start + 360.0f - end);

    // Ellipse parameters (flatter at high RPMs)
    const float ellipse_a = 100.0f; // baseline horizontal radius
    const float ellipse_b = 60.0f;  // baseline vertical radius
    const float seg_offset = 12.0f; // lift segments outward from the baseline to avoid overlap
    const float ellipse_a_seg = ellipse_a + seg_offset;
    const float ellipse_b_seg = ellipse_b + seg_offset;

    // Segment geometry (we'll step CLOCKWISE: decreasing angles from start to end)
    const int segments = 46; // density
    const float gap_deg = 0.5f; // visual gap; span computed in angle domain
    const float seg_span_deg = (sweep_cw - (segments - 1) * gap_deg) / segments;

    const QColor offColor(210, 230, 230);
    const QColor onColor(30, 30, 30);

    // Segment rendering uses variable radial length (pen width) while keeping the inner edge at a fixed offset
    const float inner_gap = 10.0f;       // fixed distance from baseline to inner edge of segments
    const float length_min = 10.0f;       // short segments at low RPM (radial length)
    const float length_max = 20.0f;      // long segments near redline
    QPen pen(Qt::black);
    pen.setCapStyle(Qt::FlatCap);

    // Mirror vertically so the arc bows downward (rainbow shape)
    painter->save();
    painter->scale(1.0f, -1.0f);

    // Build an arc-length LUT along the ellipse to space elements evenly by length
    const int samples = 512;
    std::vector<float> ang(samples);
    std::vector<float> len(samples);
    ang[0] = start;
    auto pointOnEllipse = [&](float deg) -> QPointF {
        float t = degToRad(deg);
        return QPointF(ellipse_a * std::cos(t), ellipse_b * std::sin(t));
    };
    QPointF prev = pointOnEllipse(ang[0]);
    len[0] = 0.0f;
    const float step = sweep_cw / static_cast<float>(samples - 1);
    for (int i = 1; i < samples; ++i) {
        ang[i] = start - step * static_cast<float>(i); // clockwise (decreasing)
        QPointF p = pointOnEllipse(ang[i]);
        len[i] = len[i - 1] + std::hypot(p.x() - prev.x(), p.y() - prev.y());
        prev = p;
    }
    const float totalLen = len.back();
    auto angleAtU = [&](float u) -> float {
        float target = std::clamp(u, 0.0f, 1.0f) * totalLen;
        int lo = 0, hi = samples - 1;
        while (lo + 1 < hi) {
            int mid = (lo + hi) / 2;
            if (len[mid] < target) lo = mid; else hi = mid;
        }
        float alpha = (target - len[lo]) / std::max(1e-6f, (len[hi] - len[lo]));
        return ang[lo] + (ang[hi] - ang[lo]) * alpha;
    };

    // Draw a thin baseline track under segments and ticks
    {
        QPen basePen(QColor(25, 25, 25));
        basePen.setWidthF(1.0f);
        basePen.setCapStyle(Qt::FlatCap);
        painter->setPen(basePen);
        QRectF rect(-ellipse_a, -ellipse_b, 2 * ellipse_a, 2 * ellipse_b);
        // Match segment orientation: use negative angles in mirrored space
        painter->drawArc(rect, static_cast<int>(-start * 16.0f), static_cast<int>(sweep_cw * 16.0f));
    }

    // Overlay the active proportion as dark segments; each segment is either on or off (no partials).
    const float p_now = std::clamp(_rpm / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    const int on_segments = std::clamp(static_cast<int>(std::floor(p_now * static_cast<float>(segments) + 1e-4f)), 0, segments);

    for (int i = 0; i < on_segments; ++i) {
        const float s = static_cast<float>(i) / static_cast<float>(segments - 1);
        const float a_center = angleAtU(s);
        const float a0 = a_center + seg_span_deg * 0.5f; // start angle of segment

        // Segment radial length grows with s. Keep inner edge pinned at baseline + inner_gap by
        // drawing with pen width = length and centering the stroke at inner_gap + length/2.
        const float length_px = length_min + (length_max - length_min) * s;
        const float center_offset = inner_gap + 0.5f * length_px;
        const float a_center_radius_x = ellipse_a + center_offset;
        const float a_center_radius_y = ellipse_b + center_offset;

        QRectF rect(-a_center_radius_x, -a_center_radius_y, 2 * a_center_radius_x, 2 * a_center_radius_y);
        pen.setWidthF(length_px);
        pen.setColor(onColor);
        painter->setPen(pen);
        painter->drawArc(rect, static_cast<int>(-a0 * 16.0f), static_cast<int>(-seg_span_deg * 16.0f));
    }

    painter->restore(); // undo vertical mirror
    painter->restore();
}

void MotecCdl3Tachometer::drawTicksAndLabels(QPainter* painter) {
    painter->save();

    // Ticks only at 1000 RPM with small triangles pointing inward along the varying-radius path
    const QColor tickColor(20, 20, 20);
    painter->setPen(Qt::NoPen);
    painter->setBrush(tickColor);

    // Label font (small, single digit)
    QFont labelFont = _segmentFont;
    labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() * 0.9));
    painter->setFont(labelFont);
    painter->setPen(tickColor);

    // Ellipse parameters must match the sweep bands
    const float ellipse_a = 100.0f;
    const float ellipse_b = 60.0f;

    // Place ticks starting slightly after the start to avoid crowding at zero
    // Build LUT to space ticks by arc length
    const float start = kSweepStartDeg;
    const float end = kSweepEndDeg;
    const float sweep_cw = (start >= end) ? (start - end) : (start + 360.0f - end);
    const int samples = 512;
    std::vector<float> ang(samples);
    std::vector<float> len(samples);
    ang[0] = start;
    auto pointOnEllipse = [&](float deg) -> QPointF {
        float t = degToRad(deg);
        return QPointF(ellipse_a * std::cos(t), -ellipse_b * std::sin(t)); // mirrored for downward bow
    };
    QPointF prev = pointOnEllipse(ang[0]);
    len[0] = 0.0f;
    const float step = sweep_cw / static_cast<float>(samples - 1);
    for (int i = 1; i < samples; ++i) {
        ang[i] = start - step * static_cast<float>(i);
        QPointF p = pointOnEllipse(ang[i]);
        len[i] = len[i - 1] + std::hypot(p.x() - prev.x(), p.y() - prev.y());
        prev = p;
    }
    const float totalLen = len.back();
    auto angleAtU = [&](float u) -> float {
        float target = std::clamp(u, 0.0f, 1.0f) * totalLen;
        int lo = 0, hi = samples - 1;
        while (lo + 1 < hi) { int mid = (lo + hi) / 2; if (len[mid] < target) lo = mid; else hi = mid; }
        float alpha = (target - len[lo]) / std::max(1e-6f, (len[hi] - len[lo]));
        return ang[lo] + (ang[hi] - ang[lo]) * alpha;
    };

    for (uint32_t rpm = 0; rpm <= _cfg.max_rpm; rpm += 1000) {
        float u = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        float a_deg = angleAtU(u);
        float t = degToRad(a_deg);

        // Point on baseline ellipse (where triangle bases sit), mirrored vertically
        QPointF p(ellipse_a * std::cos(t), -ellipse_b * std::sin(t));

        // Outward normal (based on gradient of implicit ellipse x^2/a^2 + y^2/b^2 = 1)
        QPointF n(p.x() / (ellipse_a * ellipse_a), p.y() / (ellipse_b * ellipse_b));
        // Normalize for outward
        float nlen = std::hypot(n.x(), n.y());
        if (nlen > 0.0f) { n.setX(n.x() / nlen); n.setY(n.y() / nlen); }

        // Tangent direction (derivative w.r.t angle parameter) with vertical mirror
        QPointF td(-ellipse_a * std::sin(t), -ellipse_b * std::cos(t));
        float tlen = std::hypot(td.x(), td.y());
        if (tlen > 0.0f) { td.setX(td.x() / tlen); td.setY(td.y() / tlen); }

        // Make triangles smaller at low RPM and larger at high RPM
        const float rp = std::clamp(static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
        const float tri_length = 3.0f;
        const float tri_half_w = 1.5f;

        QPointF tip = p + n * tri_length; // outward tip
        QPointF base1 = p + td * (-tri_half_w);
        QPointF base2 = p + td * ( tri_half_w);

        QPolygonF tri; tri << tip << base1 << base2;
        painter->drawPolygon(tri);

        // Single digit label slightly inside the tip
        QString text = QString::number(static_cast<int>(rpm / 1000));
        QPointF label_center = p - n * (tri_length + 5.0f); // place labels inside the arc
        QRectF r(0,0,14,10);
        r.moveCenter(label_center);
        painter->drawText(r, Qt::AlignCenter, text);
    }

    painter->restore();
}

#include "motec_cdl3_tachometer/moc_motec_cdl3_tachometer.cpp"


