#include "carplay_nav/carplay_nav.h"

#include <QDateTime>
#include <QPainter>
#include <QPolygonF>

#include <spdlog/spdlog.h>

#include "qt_helpers/widget_colors.h"
#include "qt_helpers/widget_fonts.h"

#include <algorithm>
#include <cmath>

using carplay_nav::ManeuverGlyph;

namespace
{

// The arrow is built in a 100x100 box centred on the origin: it rises from the
// bottom, bends once, and ends in a head. Everything is expressed as a fraction
// of that box so the widget can scale it to whatever the card leaves over.
constexpr float kArrowBox = 100.0f;
constexpr float kShaftBottomY = 42.0f;
constexpr float kArrowStrokeWidth = 13.0f;
constexpr float kArrowHeadHalfWidth = 15.0f;
constexpr float kArrowHeadLength = 24.0f;

// How far the bend sits below centre, and how long the leg after it runs. A
// sharper turn gets a shorter leg so the head stays inside the box.
constexpr float kBendY = 6.0f;
constexpr float kLegLength = 34.0f;

// Turn angle each glyph is drawn at, in degrees from straight ahead. These are
// the drawn angles, not the reported ones: a card shows a canonical "left" for
// anything the classifier called a left, rather than a faithfully-rendered 71
// degrees, because a bank of near-identical arrows is harder to read at a glance
// than four distinct ones.
float drawnAngleFor(ManeuverGlyph glyph)
{
    switch (glyph)
    {
        case ManeuverGlyph::Straight:    return 0.0f;
        case ManeuverGlyph::SlightLeft:  return -40.0f;
        case ManeuverGlyph::Left:        return -90.0f;
        case ManeuverGlyph::SharpLeft:   return -135.0f;
        case ManeuverGlyph::SlightRight: return 40.0f;
        case ManeuverGlyph::Right:       return 90.0f;
        case ManeuverGlyph::SharpRight:  return 135.0f;
        case ManeuverGlyph::UTurn:       return 180.0f;
    }
    return 0.0f;
}

QString etaClock(uint64_t epoch_sec)
{
    if (epoch_sec == 0)
    {
        return QString();
    }
    // Local time: an ETA is only useful in the timezone the car is in.
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epoch_sec)).toString("HH:mm");
}

}  // namespace

CarPlayNavWidget::CarPlayNavWidget(CarPlayNavConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
    _font_family = dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica");

    _sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayNav>>(
        _cfg.zenoh_key,
        [this](CarPlayNav::Reader reader) { onNav(reader); });
}

CarPlayNavWidget::~CarPlayNavWidget()
{
    // Drop the subscriber first so its callback cannot race destruction.
    _sub.reset();
}

void CarPlayNavWidget::onNav(CarPlayNav::Reader reader)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _active = reader.getActive();
        _road_name = QString::fromStdString(reader.getRoadName());
        _after_road_name = QString::fromStdString(reader.getAfterRoadName());
        _destination_name = QString::fromStdString(reader.getDestinationName());
        _maneuver_angle_deg = static_cast<float>(reader.getManeuverAngleDeg());
        _distance_to_maneuver_m = reader.getDistanceToManeuverM();
        _distance_remaining_m = reader.getDistanceRemainingM();
        _time_remaining_sec = reader.getTimeRemainingSec();
        _eta_epoch_sec = reader.getEtaEpochSec();
    }

    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void CarPlayNavWidget::rebuildFontsFor(qreal scale)
{
    if (_distance_fm && _road_fm && _detail_fm && qFuzzyCompare(scale, _font_scale))
    {
        return;
    }

    _font_scale = scale;

    _distance_font = QFont(_font_family);
    _distance_font.setPointSizeF(std::max<qreal>(10.0, 22.0 * scale));
    _distance_font.setBold(true);

    _road_font = QFont(_font_family);
    _road_font.setPointSizeF(std::max<qreal>(8.0, 14.0 * scale));

    _detail_font = QFont(_font_family);
    _detail_font.setPointSizeF(std::max<qreal>(6.0, 10.0 * scale));

    // QFontMetricsF has no default constructor, hence the indirection.
    _distance_fm = std::make_unique<QFontMetricsF>(_distance_font);
    _road_fm = std::make_unique<QFontMetricsF>(_road_font);
    _detail_fm = std::make_unique<QFontMetricsF>(_detail_font);
}

QPainterPath CarPlayNavWidget::arrowPath(ManeuverGlyph glyph)
{
    const float angle_deg = drawnAngleFor(glyph);
    const float angle_rad = angle_deg * static_cast<float>(M_PI) / 180.0f;

    // "Up" is -y in Qt's coordinates, so the leg direction is (sin, -cos) of the
    // turn angle: 0 goes straight up, +90 turns to the right.
    const QPointF bend(0.0f, kBendY);
    const QPointF direction(std::sin(angle_rad), -std::cos(angle_rad));

    QPainterPath path;

    if (glyph == ManeuverGlyph::UTurn)
    {
        // A U-turn is not a bend, it is a hook: up, round, and back down the
        // other side. Drawing it as a 180-degree "turn" would put the head on
        // top of the shaft.
        const float radius = 20.0f;
        path.moveTo(-radius, kShaftBottomY);
        path.lineTo(-radius, kBendY);
        path.arcTo(QRectF(-radius, kBendY - radius, radius * 2.0f, radius * 2.0f), 180.0f, -180.0f);
        path.lineTo(radius, kBendY + kLegLength * 0.4f);
        return path;
    }

    path.moveTo(0.0f, kShaftBottomY);
    path.lineTo(bend);
    if (glyph != ManeuverGlyph::Straight)
    {
        path.lineTo(bend + direction * kLegLength);
    }
    else
    {
        // Straight ahead is one long shaft rather than a shaft plus a leg.
        path.lineTo(QPointF(0.0f, kBendY - kLegLength));
    }
    return path;
}

void CarPlayNavWidget::paintEvent(QPaintEvent* /*event*/)
{
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        active = _active;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds(0, 0, width(), height());

    const QColor background = dashboard::toQColor(_cfg.background_color, Qt::transparent);
    if (background.alpha() > 0)
    {
        p.fillRect(bounds, background);
    }

    const qreal s = std::max<qreal>(0.4, bounds.height() / 110.0);
    rebuildFontsFor(s);

    if (active)
    {
        paintGuidance(p, bounds);
    }
    else
    {
        paintIdle(p, bounds);
    }
}

void CarPlayNavWidget::paintIdle(QPainter& p, const QRectF& bounds)
{
    p.setFont(_road_font);
    p.setPen(dashboard::toQColor(_cfg.detail_color));
    p.drawText(bounds, Qt::AlignCenter, QString::fromStdString(_cfg.idle_text));
}

void CarPlayNavWidget::paintGuidance(QPainter& p, const QRectF& bounds)
{
    QString road, after_road, destination;
    float angle = 0.0f;
    float to_maneuver = 0.0f;
    float remaining = 0.0f;
    float time_remaining = 0.0f;
    uint64_t eta = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        road = _road_name;
        after_road = _after_road_name;
        destination = _destination_name;
        angle = _maneuver_angle_deg;
        to_maneuver = _distance_to_maneuver_m;
        remaining = _distance_remaining_m;
        time_remaining = _time_remaining_sec;
        eta = _eta_epoch_sec;
    }

    const QFontMetricsF& distance_fm = *_distance_fm;
    const QFontMetricsF& road_fm = *_road_fm;
    const QFontMetricsF& detail_fm = *_detail_fm;

    // The trip summary is a strip along the bottom; the turn card gets whatever
    // is left. Reserve it first so the arrow is sized against the right box.
    QRectF card = bounds;
    QRectF summary;
    if (_cfg.show_trip_summary)
    {
        const qreal summary_height = detail_fm.height() * 1.4;
        summary = QRectF(bounds.left(), bounds.bottom() - summary_height,
                         bounds.width(), summary_height);
        card.setBottom(summary.top());
    }

    // Arrow on the left, in a square as tall as the card allows.
    const qreal arrow_side = std::min(card.height() * 0.86, card.width() * 0.34);
    const QRectF arrow_box(card.left() + card.height() * 0.07,
                           card.top() + (card.height() - arrow_side) / 2.0,
                           arrow_side, arrow_side);

    const ManeuverGlyph glyph = carplay_nav::glyphForAngle(angle);
    const QColor arrow_color = dashboard::toQColor(_cfg.arrow_color);

    p.save();
    p.translate(arrow_box.center());
    p.scale(arrow_side / kArrowBox, arrow_side / kArrowBox);

    QPen arrow_pen(arrow_color);
    arrow_pen.setWidthF(kArrowStrokeWidth);
    arrow_pen.setCapStyle(Qt::FlatCap);
    arrow_pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(arrow_pen);
    p.setBrush(Qt::NoBrush);

    const QPainterPath shaft = arrowPath(glyph);
    p.drawPath(shaft);

    // Head at the end of the path, pointing the way the last segment ran. Taking
    // the direction from the path rather than recomputing it keeps the head
    // aligned with the U-turn hook too, whose end direction is not the turn
    // angle.
    const qreal end_pct = 1.0;
    const QPointF tip = shaft.pointAtPercent(end_pct);
    const qreal head_angle_deg = shaft.angleAtPercent(end_pct);
    const qreal head_angle_rad = -head_angle_deg * M_PI / 180.0;
    const QPointF forward(std::cos(head_angle_rad), std::sin(head_angle_rad));
    const QPointF side(-forward.y(), forward.x());

    QPolygonF head;
    head << tip + forward * kArrowHeadLength
         << tip + side * kArrowHeadHalfWidth
         << tip - side * kArrowHeadHalfWidth;
    p.setPen(Qt::NoPen);
    p.setBrush(arrow_color);
    p.drawPolygon(head);
    p.restore();

    // Text column to the right of the arrow.
    QRectF text_area = card;
    text_area.setLeft(arrow_box.right() + card.height() * 0.08);
    text_area.setRight(card.right() - card.height() * 0.05);

    qreal y = text_area.top() + card.height() * 0.10;

    p.setFont(_distance_font);
    p.setPen(dashboard::toQColor(_cfg.distance_color));
    p.drawText(QRectF(text_area.left(), y, text_area.width(), distance_fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               QString::fromStdString(
                   carplay_nav::formatDistance(to_maneuver, _cfg.imperial_units)));
    y += distance_fm.height();

    // The road being turned ONTO is the one that matters at the turn; the road
    // currently on is context and gets the smaller line.
    const QString primary = after_road.isEmpty() ? road : after_road;
    if (!primary.isEmpty())
    {
        p.setFont(_road_font);
        p.setPen(dashboard::toQColor(_cfg.road_color));
        p.drawText(QRectF(text_area.left(), y, text_area.width(), road_fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   road_fm.elidedText(primary, Qt::ElideRight, text_area.width()));
        y += road_fm.height();
    }

    if (!after_road.isEmpty() && !road.isEmpty() && road != after_road)
    {
        p.setFont(_detail_font);
        p.setPen(dashboard::toQColor(_cfg.detail_color));
        p.drawText(QRectF(text_area.left(), y, text_area.width(), detail_fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   detail_fm.elidedText(QStringLiteral("on ") + road, Qt::ElideRight,
                                        text_area.width()));
    }

    if (!_cfg.show_trip_summary)
    {
        return;
    }

    p.setFont(_detail_font);
    p.setPen(dashboard::toQColor(_cfg.detail_color));

    const QString eta_text = etaClock(eta);
    QString left_text = QString::fromStdString(carplay_nav::formatDuration(time_remaining)) +
                        QStringLiteral("  ·  ") +
                        QString::fromStdString(
                            carplay_nav::formatDistance(remaining, _cfg.imperial_units));
    if (!destination.isEmpty())
    {
        left_text = destination + QStringLiteral("  ·  ") + left_text;
    }

    const QRectF summary_text = summary.adjusted(bounds.height() * 0.05, 0,
                                                 -bounds.height() * 0.05, 0);
    p.drawText(summary_text, Qt::AlignLeft | Qt::AlignVCenter,
               detail_fm.elidedText(left_text, Qt::ElideRight,
                                    summary_text.width() - detail_fm.horizontalAdvance(eta_text) -
                                        detail_fm.horizontalAdvance(QStringLiteral("  "))));
    if (!eta_text.isEmpty())
    {
        p.drawText(summary_text, Qt::AlignRight | Qt::AlignVCenter, eta_text);
    }
}

#include "carplay_nav/moc_carplay_nav.cpp"
