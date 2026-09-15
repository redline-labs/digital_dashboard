#include "center_bar/center_bar.h"

#include <QPainter>
#include <QFontMetricsF>

#include <spdlog/spdlog.h>

#include "qt_helpers/widget_colors.h"
#include "qt_helpers/widget_fonts.h"

#include <algorithm>
#include <cmath>

namespace
{
// Proportions of the widget height, so the strip scales with whatever box the
// layout gives it.
constexpr float kTrackHeightFraction = 0.30f;
constexpr float kMarkerWidthFraction = 0.16f;
constexpr float kLabelPtBase = 9.0f;
constexpr float kLabelPtMin = 6.0f;
}  // namespace

CenterBarWidget::CenterBarWidget(const CenterBarConfig_t& cfg, QWidget* parent) :
    QWidget(parent),
    _cfg{cfg}
{
    const QString family = qt_helpers::loadResourceFont(":/fonts/futura.ttf", "Helvetica");
    _label_font = QFont(family, 9, QFont::DemiBold);

    _expression_parser = dashboard::makeExpressionSubscription<double>(
        _cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
        this, &CenterBarWidget::setValue, "center bar");
}

void CenterBarWidget::setValue(double value)
{
    if (!std::isfinite(value))
    {
        // A non-finite reading would sail through the clamp below -- both of
        // std::clamp's comparisons are false for NaN -- and land in the marker
        // position as a NaN x coordinate.
        return;
    }

    const double clamped = std::clamp<double>(value, -_cfg.range, _cfg.range);
    if (clamped == _value)
    {
        return;
    }
    _value = clamped;
    update();
}

void CenterBarWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds(0, 0, width(), height());

    QFont label_font = _label_font;
    label_font.setPointSizeF(
        std::max<qreal>(kLabelPtMin, kLabelPtBase * (bounds.height() / 24.0)));
    p.setFont(label_font);
    const QFontMetricsF fm(label_font);

    const QString left_label = QString::fromStdString(_cfg.left_label);
    const QString right_label = QString::fromStdString(_cfg.right_label);

    // Labels bracket the track, so reserve their width before laying it out.
    const qreal gap = bounds.height() * 0.25;
    const qreal left_width = left_label.isEmpty() ? 0.0 : fm.horizontalAdvance(left_label) + gap;
    const qreal right_width = right_label.isEmpty() ? 0.0 : fm.horizontalAdvance(right_label) + gap;

    p.setPen(qt_helpers::toQColor(_cfg.label_color));
    if (!left_label.isEmpty())
    {
        p.drawText(QRectF(bounds.left(), bounds.top(), left_width - gap, bounds.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, left_label);
    }
    if (!right_label.isEmpty())
    {
        p.drawText(QRectF(bounds.right() - right_width + gap, bounds.top(),
                          right_width - gap, bounds.height()),
                   Qt::AlignRight | Qt::AlignVCenter, right_label);
    }

    const qreal track_height = bounds.height() * kTrackHeightFraction;
    const QRectF track(bounds.left() + left_width,
                       bounds.center().y() - track_height / 2.0,
                       bounds.width() - left_width - right_width,
                       track_height);
    if (track.width() <= 0.0)
    {
        // The labels ate the whole widget. Drawing the marker against a
        // zero-or-negative width track puts it outside the widget entirely.
        return;
    }

    p.setPen(Qt::NoPen);
    p.setBrush(qt_helpers::toQColor(_cfg.track_color));
    p.drawRoundedRect(track, track_height / 2.0, track_height / 2.0);

    // Centre tick: the zero the marker is read against.
    const qreal centre_x = track.center().x();
    QPen tick_pen(qt_helpers::toQColor(_cfg.tick_color));
    tick_pen.setWidthF(std::max<qreal>(1.0, bounds.height() * 0.04));
    p.setPen(tick_pen);
    p.drawLine(QPointF(centre_x, track.top() - track_height * 0.35),
               QPointF(centre_x, track.bottom() + track_height * 0.35));

    // Marker. The fraction is already clamped in setValue, but clamp again here
    // so a value set before the config was validated cannot draw off the track.
    const double fraction = std::clamp<double>(_value / _cfg.range, -1.0, 1.0);
    const qreal marker_width = std::max<qreal>(4.0, bounds.width() * kMarkerWidthFraction * 0.25);
    const qreal travel = (track.width() - marker_width) / 2.0;
    const QRectF marker(centre_x + fraction * travel - marker_width / 2.0,
                        track.top(), marker_width, track.height());

    const bool good = _cfg.negative_is_good ? (_value <= 0.0) : (_value >= 0.0);
    p.setPen(Qt::NoPen);
    p.setBrush(qt_helpers::toQColor(good ? _cfg.good_color : _cfg.bad_color));
    p.drawRoundedRect(marker, marker_width / 3.0, marker_width / 3.0);
}

#include "center_bar/moc_center_bar.cpp"
