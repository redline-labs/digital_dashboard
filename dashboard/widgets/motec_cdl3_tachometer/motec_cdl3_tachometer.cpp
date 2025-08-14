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
#include "helpers/helpers.h"

namespace {

// Define the sweep angles following the MoTeC CDL3 top arc feel
// Start on the LEFT shoulder and sweep CLOCKWISE to the RIGHT shoulder
constexpr float kSweepStartDeg = 170.0f; // start at left shoulder (0 RPM)
constexpr float kSweepEndDeg   = 70.0f;  // end at right shoulder  (max RPM)
constexpr float kEllipseA = 100.0f;
constexpr float kEllipseB = 60.0f;

// Piecewise angle mapping to imitate a slightly non-linear scale seen on CDL3
// We'll bias low range to sweep faster (bigger angle per 1000) and compress near top.
}

MotecCdl3Tachometer::MotecCdl3Tachometer(const MotecCdl3TachometerConfig_t& cfg, QWidget* parent)
    : QWidget(parent), _cfg{cfg}, _rpm{0.0f} {
    // Load segmented display font (DSEG)
    int dsegId = QFontDatabase::addApplicationFont(":/fonts/DSEG7Classic-Bold.ttf");
    if (dsegId == -1)
    {
        _segmentFont = QFont("Helvetica", 10, QFont::Bold);
        SPDLOG_WARN("Failed to load DSEG font, falling back to system font");
    }
    else
    {
        const QString family = QFontDatabase::applicationFontFamilies(dsegId).at(0);
        _segmentFont = QFont(family, 10, QFont::Bold);
    }

    // Optional expression parser hookup
    try
    {
        _expression_parser = std::make_unique<expression_parser::ExpressionParser>(
            _cfg.schema_type, _cfg.rpm_expression, _cfg.zenoh_key);
        if (_expression_parser->isValid())
        {
            _expression_parser->setResultCallback<float>([this](float rpm)
            {
                QMetaObject::invokeMethod(this, "setRpm", Qt::QueuedConnection, Q_ARG(float, rpm));
            });
        } else {
            _expression_parser.reset();
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("MotecCdl3Tachometer: expression parser init failed: {}", e.what());
        _expression_parser.reset();
    }
    // Precompute LUT and static geometry for even spacing and fast drawing
    buildArcLUT();
    buildStaticGeometry();
}

void MotecCdl3Tachometer::setZenohSession(std::shared_ptr<zenoh::Session> session) {
    _zenoh_session = std::move(session);
}

void MotecCdl3Tachometer::setRpm(float rpm) {
    _rpm = std::clamp(rpm, 0.0f, static_cast<float>(_cfg.max_rpm));
    update();
}

void MotecCdl3Tachometer::paintEvent(QPaintEvent* e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int side = qMin(width(), height());
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(side / 220.0, side / 220.0); // normalize to 220x220 box, slightly larger

    drawSweepBands(&p);
    drawTicksAndLabels(&p);
}

void MotecCdl3Tachometer::buildArcLUT()
{
    constexpr float sweep_cw = (kSweepStartDeg >= kSweepEndDeg) ? (kSweepStartDeg - kSweepEndDeg) : (kSweepStartDeg + 360.0f - kSweepEndDeg);

    _lutAngles[0] = kSweepStartDeg;
    auto pointOnEllipse = [&](float deg) -> QPointF {
        float t = degrees_to_radians(deg);
        return QPointF(kEllipseA * std::cos(t), kEllipseB * std::sin(t));
    };
    QPointF prev = pointOnEllipse(_lutAngles[0]);
    _lutLengths[0] = 0.0f;
    const float step = sweep_cw / static_cast<float>(kLutSamples - 1);
    for (int i = 1; i < kLutSamples; ++i)
    {
        _lutAngles[i] = kSweepStartDeg - step * static_cast<float>(i);
        QPointF p = pointOnEllipse(_lutAngles[i]);
        _lutLengths[i] = _lutLengths[i - 1] + std::hypot(p.x() - prev.x(), p.y() - prev.y());
        prev = p;
    }
}

float MotecCdl3Tachometer::angleAtU(float u) const
{
    const float totalLen = _lutLengths.back();
    float target = std::clamp(u, 0.0f, 1.0f) * totalLen;
    int lo = 0, hi = kLutSamples - 1;

    while (lo + 1 < hi)
    {
        int mid = (lo + hi) / 2;
        if (_lutLengths[mid] < target)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }

    float alpha = (target - _lutLengths[lo]) / std::max(1e-6f, (_lutLengths[hi] - _lutLengths[lo]));
    return _lutAngles[lo] + (_lutAngles[hi] - _lutAngles[lo]) * alpha;
}

void MotecCdl3Tachometer::buildStaticGeometry()
{
    constexpr float inner_gap = 5.0f;   // Gap between segments and baseline.
    constexpr float length_min = 10.0f; // min segment thickness (pen width)
    constexpr float length_max = 20.0f; // max segment thickness (pen width)

    // Compute total baseline arc length using LUT
    const float totalLen = _lutLengths.back();

    // Desired fixed pixel gap between segments along the baseline path
    constexpr float gap_px = 0.1f;
    constexpr float total_gaps = static_cast<float>(kSegments - 1) * gap_px;
    const float alloc_for_segments = std::max(0.0f, totalLen - total_gaps);
    const float seg_len_px = (kSegments > 0) ? (alloc_for_segments / static_cast<float>(kSegments)) : 0.0f;

    // Helper: map arc-length s (0..totalLen) to an angle using LUT (inverse of _lutLengths)
    auto angleAtArcLen = [&](float arcLen) -> float {
        float target = std::clamp(arcLen, 0.0f, totalLen);
        int lo = 0, hi = kLutSamples - 1;
        while (lo + 1 < hi) {
            int mid = (lo + hi) / 2;
            if (_lutLengths[mid] < target) lo = mid; else hi = mid;
        }
        float denom = std::max(1e-6f, (_lutLengths[hi] - _lutLengths[lo]));
        float alpha = (target - _lutLengths[lo]) / denom;
        return _lutAngles[lo] + (_lutAngles[hi] - _lutAngles[lo]) * alpha;
    };

    // Build per-segment start angle and span (deg) using constant baseline pixel gap
    float curArc = 0.0f;
    for (int i = 0; i < kSegments; ++i) {
        float segStartArc = curArc;
        float segEndArc = std::min(totalLen, segStartArc + seg_len_px);
        float a_start = angleAtArcLen(segStartArc);
        float a_end   = angleAtArcLen(segEndArc);

        // Moving clockwise from kSweepStartDeg to kSweepEndDeg, angles decrease.
        // We store start angle (greater) and negative span for QPainter drawArc in mirrored space
        float span_deg = a_end - a_start; // will be negative magnitude
        _segmentStartAngles[i] = a_start;
        _segmentSpanDeg[i] = span_deg;

        // Thickness and radial rect per segment (same as before, but keep center offset constant)
        const float s = static_cast<float>(i) / static_cast<float>(kSegments - 1);
        const float length_px = length_min + (length_max - length_min) * s;
        _segmentLengthPx[i] = length_px;
        const float center_offset = inner_gap + 0.5f * length_px;
        _segmentRectAX[i] = kEllipseA + center_offset;
        _segmentRectBY[i] = kEllipseB + center_offset;

        // Advance with gap after this segment (except after last, but harmless)
        curArc = segEndArc + gap_px;
    }

    // Precompute tick angles at 1000 RPM using LUT proportion mapping
    _tickAngles.clear();
    for (uint32_t rpm = 0; rpm <= _cfg.max_rpm; rpm += 1000) {
        float u = static_cast<float>(rpm) / static_cast<float>(_cfg.max_rpm);
        _tickAngles.push_back(angleAtU(u));
    }
}

// Non-linear mapping from RPM proportion to angle. We use a simple quadratic ease-out near top.
float MotecCdl3Tachometer::mapRpmToAngleDeg(float rpm) const
{
    const float proportion = std::clamp(rpm / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    // ease: y = 1 - (1 - x)^2 -> fast at start, slow near end (matches perceived spacing)
    const float eased = 1.0f - (1.0f - proportion) * (1.0f - proportion);
    constexpr float sweep_cw = (kSweepStartDeg >= kSweepEndDeg)
                               ? (kSweepStartDeg - kSweepEndDeg)
                               : (kSweepStartDeg + 360.0f - kSweepEndDeg);
    // Decrease angle with rpm so sweep proceeds CLOCKWISE from left to right
    return kSweepStartDeg - sweep_cw * eased;
}

// The visible band radius varies with angle to mimic the CDL3 taper (shallower near start, deeper near middle)
float MotecCdl3Tachometer::mapAngleToRadius(float angle_deg) const
{
    // Normalize to 0..1 across the sweep (clockwise)
    constexpr float sweep_cw = (kSweepEndDeg >= kSweepStartDeg) ? (kSweepEndDeg - kSweepStartDeg)
                                                           : (kSweepEndDeg + 360.0f - kSweepStartDeg);
    float pos = angle_deg - kSweepStartDeg;
    if (pos < 0.0f)
    {
        pos += 360.0f;
    }
    const float t = std::clamp(pos / sweep_cw, 0.0f, 1.0f);

    // Flatten at higher RPMs by increasing radius with t (non-constant radius path)
    // Start narrower (more curved), end wider (flatter)
    constexpr float base_inner = 60.0f;
    constexpr float base_outer = 86.0f; // target outer bound for the channel center

    // Use a smoothstep-like curve and a small mid bulge to avoid a perfectly circular feel
    const float grow = std::pow(t, 0.55f); // faster growth early, gentle near end
    const float bulge = 0.06f * std::sin(static_cast<float>(std::numbers::pi) * t);
    const float center = base_inner + (base_outer - base_inner) * (0.70f + 0.25f * grow + bulge);
    return center;
}

void MotecCdl3Tachometer::drawSweepBands(QPainter* painter)
{
    painter->save();

    // Elliptical path to mimic CDL3: top arc flatter than sides
    constexpr float sweep_cw = (kSweepStartDeg >= kSweepEndDeg) ? (kSweepStartDeg - kSweepEndDeg) : (kSweepStartDeg + 360.0f - kSweepEndDeg);

    constexpr QColor onColor(30, 30, 30);

    // Segment rendering uses variable radial length (pen width) while keeping the inner edge at a fixed offset
    QPen pen(Qt::black);
    pen.setCapStyle(Qt::FlatCap);

    // Mirror vertically so the arc bows downward (rainbow shape)
    painter->save();
    painter->scale(1.0f, -1.0f);

    // Use precomputed LUT

    // Draw a thin baseline track under segments and ticks
    {
        QPen basePen(QColor(25, 25, 25));
        basePen.setWidthF(1.0f);
        basePen.setCapStyle(Qt::FlatCap);
        painter->setPen(basePen);
        QRectF rect(-kEllipseA, -kEllipseB, 2 * kEllipseA, 2 * kEllipseB);
        // Match segment orientation: use negative angles in mirrored space
        painter->drawArc(rect, static_cast<int>(-kSweepStartDeg * 16.0f), static_cast<int>(sweep_cw * 16.0f));
    }

    // Overlay the active proportion as dark segments; each segment is either on or off (no partials).
    const float p_now = std::clamp(_rpm / static_cast<float>(_cfg.max_rpm), 0.0f, 1.0f);
    const int on_segments = std::clamp(static_cast<int>(std::floor(p_now * static_cast<float>(kSegments) + 1e-4f)), 0, kSegments);

    for (int i = 0; i < on_segments; ++i)
    {
        const float a0 = _segmentStartAngles[i];
        const float span = _segmentSpanDeg[i];
        QRectF rect(-_segmentRectAX[i], -_segmentRectBY[i], 2 * _segmentRectAX[i], 2 * _segmentRectBY[i]);
        pen.setWidthF(_segmentLengthPx[i]);
        pen.setColor(onColor);
        painter->setPen(pen);
        painter->drawArc(rect, static_cast<int>(-a0 * 16.0f), static_cast<int>(-std::abs(span) * 16.0f));
    }

    painter->restore(); // undo vertical mirror
    painter->restore();
}

void MotecCdl3Tachometer::drawTicksAndLabels(QPainter* painter) {
    painter->save();

    // Ticks only at 1000 RPM with small triangles pointing inward along the varying-radius path
    constexpr QColor tickColor(20, 20, 20);
    painter->setPen(Qt::NoPen);
    painter->setBrush(tickColor);

    // Label font (small, single digit)
    QFont labelFont = _segmentFont;
    labelFont.setPointSizeF(5.0f);
    painter->setFont(labelFont);
    painter->setPen(tickColor);

    // Place ticks using precomputed LUT

    for (size_t i = 0; i < _tickAngles.size(); ++i) {
        float a_deg = _tickAngles[i];
        float t = degrees_to_radians(a_deg);

        // Point on baseline ellipse (where triangle bases sit), mirrored vertically
        QPointF p(kEllipseA * std::cos(t), -kEllipseB * std::sin(t));

        // Outward normal (based on gradient of implicit ellipse x^2/a^2 + y^2/b^2 = 1)
        QPointF n(p.x() / (kEllipseA * kEllipseA), p.y() / (kEllipseB * kEllipseB));
        // Normalize for outward
        float nlen = std::hypot(n.x(), n.y());
        if (nlen > 0.0f) { n.setX(n.x() / nlen); n.setY(n.y() / nlen); }

        // Tangent direction (derivative w.r.t angle parameter) with vertical mirror
        QPointF td(-kEllipseA * std::sin(t), -kEllipseB * std::cos(t));
        float tlen = std::hypot(td.x(), td.y());
        if (tlen > 0.0f) { td.setX(td.x() / tlen); td.setY(td.y() / tlen); }

        // Make triangles smaller at low RPM and larger at high RPM
        constexpr float tri_length = 2.0f;
        constexpr float tri_half_w = 1.0f;

        QPointF tip = p + n * tri_length; // outward tip
        QPointF base1 = p + td * (-tri_half_w);
        QPointF base2 = p + td * ( tri_half_w);

        QPolygonF tri;
        tri << tip << base1 << base2;
        painter->drawPolygon(tri);

        // Single digit label slightly inside the tip
        QString text = QString::number(static_cast<int>(i));
        QPointF label_center = p - n * (tri_length + 5.0f); // place labels inside the arc
        QRectF r(0,0,14,10);
        r.moveCenter(label_center);
        painter->drawText(r, Qt::AlignCenter, text);
    }

    painter->restore();
}

#include "motec_cdl3_tachometer/moc_motec_cdl3_tachometer.cpp"
