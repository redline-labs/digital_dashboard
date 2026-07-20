#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include <QPaintEvent>
#include <QFontMetrics>
#include <QSvgRenderer>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "dashboard/gauge_painting.h"
#include "dashboard/widget_fonts.h"
#include "helpers/unit_conversion.h"

#include <cmath>
#include <vector>
#include <algorithm>

// Shared drawing constants to keep visual consistency across sub-gauges.
// Adjusting these values changes the overall geometry/appearance.
namespace
{
    // Logical canvas size used for uniform device scaling across widget sizes.
    constexpr float kCanvasLogicalSize = 200.0f;

    // Main circular background radius in logical units.
    constexpr float kMainGaugeRadius = 100.0f;

    // Radial distance from center to each sub-gauge's pivot (their local center).
    constexpr float kSubGaugeRadius = 45.0f;

    // Radial position for tick marks drawn around the outer perimeter.
    constexpr float kOuterTickRadius = 98.0f;

    // Each sub-gauge sweeps a quarter-circle for clarity and compactness.
    constexpr float kGaugeSpanDegrees = 90.0f;

    // Needle styling shared by all sub-gauges to match cluster aesthetics.
    constexpr QColor kNeedleColor(255, 165, 0); // Orange needle for brand consistency
    constexpr float kNeedleBaseWidth = 3.0f;
    constexpr float kNeedleTipWidth  = 2.0f;
    constexpr float kNeedleLength    = 50.0f;

    // Typography used for tick labels and unit labels.
    constexpr float kLabelFontPt = 9.0f;
    constexpr float kUnitFontPt  = 10.0f;

    // Tick length drawn radially inward from the outer perimeter.
    constexpr float kTickLength = 8.0f;

    // Tick thicknesses used for major/minor ticks.
    constexpr float kTickThicknessMajor = 3.0f;
    constexpr float kTickThicknessMinor = 2.0f;

    // Label radius relative to outer ticks so labels sit just inside.
    constexpr float kLabelRadius = 82.0f;

    // Offsets to position the coolant unit text (°C) near the high end.
    constexpr float kCoolantUnitOffsetX = -10.0f;
    constexpr float kCoolantUnitOffsetY = -15.0f;

    // Radius of center pivot dot for each sub-gauge needle.
    constexpr float kPivotRadius = 6.0f;

    // Icon sizes/offsets used below gauge pivots.
    constexpr float kGaugeIconDefaultSize = 20.0f;
    constexpr float kGaugeIconDefaultOffsetY = 10.0f;
    constexpr float kOilIconWidth = 24.0f;
    constexpr float kOilIconHeight = 12.0f;
    constexpr float kOilIconOffsetXFromCenter = 20.0f;
    constexpr float kOilIconOffsetY = 12.0f;

    // Fuel gauge left triangle marker geometry.
    constexpr float kFuelTriangleOffsetX = 46.0f;
    constexpr float kFuelTriangleOffsetY = 30.0f;
    constexpr float kFuelTriangleSize = 8.0f;

    enum class GaugeOrientation { Up, Right, Down, Left };
}

struct GaugeOrientationInfo
{
    float orientationAngleDeg = 0.0f;
    bool rotateClockwiseNegative = true;
    bool invertValueMapping = false;
    float projectionSign = 1.0f;
};

static GaugeOrientationInfo getOrientationInfo(GaugeOrientation orientation)
{
    GaugeOrientationInfo info;
    switch (orientation)
    {
        case GaugeOrientation::Up:
            info.orientationAngleDeg = 90.0f;
            info.rotateClockwiseNegative = true;
            info.invertValueMapping = true;
            info.projectionSign = -1.0f;
            break;
        case GaugeOrientation::Right:
            info.orientationAngleDeg = 0.0f;
            info.rotateClockwiseNegative = true;
            info.invertValueMapping = false;
            info.projectionSign = 1.0f;
            break;
        case GaugeOrientation::Down:
            // Qt's y axis points down, so screen-down is 90 degrees.
            info.orientationAngleDeg = 90.0f;
            info.rotateClockwiseNegative = false;
            info.invertValueMapping = true;
            info.projectionSign = 1.0f;
            break;
        case GaugeOrientation::Left:
            info.orientationAngleDeg = 180.0f;
            info.rotateClockwiseNegative = false;
            info.invertValueMapping = false;
            info.projectionSign = 1.0f;
            break;
    }
    return info;
}

static void drawGaugeBase(
    QPainter* painter,
    float centerX,
    float centerY,
    GaugeOrientation orientation,
    int numTicks,
    bool alternateTickThickness,
    const std::vector<const char*>& labels)
{
    painter->save();
    const auto info = getOrientationInfo(orientation);
    const float gaugeStartAngle = info.orientationAngleDeg - (kGaugeSpanDegrees / 2.0f);

    for (int i = 0; i < numTicks; ++i)
    {
        float valueRatio = static_cast<float>(i) / (numTicks - 1);
        float mapped = info.invertValueMapping ? (1.0f - valueRatio) : valueRatio;
        float tickAngle = gaugeStartAngle + (mapped * kGaugeSpanDegrees);
        float tickAngleRad = degrees_to_radians(tickAngle);

        float projectedAngle = std::atan2(centerY + info.projectionSign * kNeedleLength * std::sin(tickAngleRad),
                                          centerX + info.projectionSign * kNeedleLength * std::cos(tickAngleRad));

        QPointF tickOuter(kOuterTickRadius * std::cos(projectedAngle),
                          kOuterTickRadius * std::sin(projectedAngle));

        QPointF dirToSubGaugeCenter = QPointF(centerX, centerY) - tickOuter;
        float dirLength = std::sqrt(dirToSubGaugeCenter.x() * dirToSubGaugeCenter.x() +
                                     dirToSubGaugeCenter.y() * dirToSubGaugeCenter.y());
        if (dirLength > 0.0f) {
            dirToSubGaugeCenter.setX(dirToSubGaugeCenter.x() / dirLength);
            dirToSubGaugeCenter.setY(dirToSubGaugeCenter.y() / dirLength);
        }

        QPointF tickInner = tickOuter + dirToSubGaugeCenter * kTickLength;
        float thickness = alternateTickThickness ? (i % 2 == 0 ? kTickThicknessMajor : kTickThicknessMinor)
                                                 : kTickThicknessMajor;
        QPen tickPen(Qt::white, thickness);
        painter->setPen(tickPen);
        painter->drawLine(tickOuter, tickInner);
    }

    if (!labels.empty())
    {
        painter->save();
        QFont labelFont(painter->font());
        labelFont.setPointSizeF(kLabelFontPt);
        painter->setFont(labelFont);
        painter->setPen(Qt::white);
        QFontMetricsF fm(labelFont);

        for (int i = 0; i < numTicks && i < static_cast<int>(labels.size()); ++i)
        {
            if (labels[i] == nullptr) continue;
            float valueRatio = static_cast<float>(i) / (numTicks - 1);
            float mapped = info.invertValueMapping ? (1.0f - valueRatio) : valueRatio;
            float labelAngle = gaugeStartAngle + (mapped * kGaugeSpanDegrees);
            float labelAngleRad = degrees_to_radians(labelAngle);
            float projectedAngle = std::atan2(centerY + info.projectionSign * kNeedleLength * std::sin(labelAngleRad),
                                              centerX + info.projectionSign * kNeedleLength * std::cos(labelAngleRad));
            QPointF labelPos(kLabelRadius * std::cos(projectedAngle),
                             kLabelRadius * std::sin(projectedAngle));
            QString labelText = QString::fromUtf8(labels[i]);
            QRectF textRect = fm.boundingRect(labelText);
            textRect.moveCenter(labelPos);
            painter->drawText(textRect, Qt::AlignCenter, labelText);
        }

        painter->restore();
    }

    painter->restore();
}

static void drawGaugeNeedle(
    QPainter* painter,
    const sub_gauge_config_t& gauge,
    float centerX,
    float centerY,
    GaugeOrientation orientation,
    float currentValue)
{
    const auto info = getOrientationInfo(orientation);
    const float gaugeStartAngle = info.orientationAngleDeg - (kGaugeSpanDegrees / 2.0f);

    float valueRatio = 0.0f;
    if (gauge.max_value != gauge.min_value)
    {
        float clamped = std::clamp(currentValue, gauge.min_value, gauge.max_value);
        valueRatio = (clamped - gauge.min_value) / (gauge.max_value - gauge.min_value);
    }
    valueRatio = std::clamp(valueRatio, 0.0f, 1.0f);
    float mappedNeedle = info.invertValueMapping ? (1.0f - valueRatio) : valueRatio;
    float needleAngle = gaugeStartAngle + (mappedNeedle * kGaugeSpanDegrees);

    painter->save();
    painter->translate(centerX, centerY);
    painter->rotate(info.rotateClockwiseNegative ? (-1.0f * needleAngle) : needleAngle);
    gauge_paint::drawTaperedNeedle(*painter, kNeedleLength, kNeedleBaseWidth, kNeedleTipWidth, kNeedleColor);
    painter->restore();

    painter->save();
    painter->translate(centerX, centerY);
    gauge_paint::drawPivot(*painter, kPivotRadius);
    painter->restore();
}

Mercedes190EClusterGauge::Mercedes190EClusterGauge(const Mercedes190EClusterGaugeConfig_t& cfg, QWidget *parent):
  dashboard::CachedPaintWidget(parent),
  m_config(cfg),
  fuel_gauge_current_value_(0.0f),
  oil_pressure_gauge_current_value_(0.0f),
  coolant_temperature_gauge_current_value_(0.0f),
  economy_gauge_current_value_(0.0f)
{
    m_fontFamily = dashboard::loadResourceFont(":/fonts/futura.ttf", "sans-serif");

    top_gauge_expression_parser_ = dashboard::makeExpressionSubscription<float>(
        m_config.fuel_gauge.schema_type, m_config.fuel_gauge.value_expression, m_config.fuel_gauge.zenoh_key,
        this, &Mercedes190EClusterGauge::setFuelGaugeValue, "cluster top gauge");

    right_gauge_expression_parser_ = dashboard::makeExpressionSubscription<float>(
        m_config.right_gauge.schema_type, m_config.right_gauge.value_expression, m_config.right_gauge.zenoh_key,
        this, &Mercedes190EClusterGauge::setOilPressureGaugeValue, "cluster right gauge");

    bottom_gauge_expression_parser_ = dashboard::makeExpressionSubscription<float>(
        m_config.bottom_gauge.schema_type, m_config.bottom_gauge.value_expression, m_config.bottom_gauge.zenoh_key,
        this, &Mercedes190EClusterGauge::setEconomyGaugeValue, "cluster bottom gauge");

    left_gauge_expression_parser_ = dashboard::makeExpressionSubscription<float>(
        m_config.left_gauge.schema_type, m_config.left_gauge.value_expression, m_config.left_gauge.zenoh_key,
        this, &Mercedes190EClusterGauge::setCoolantTemperatureGaugeValue, "cluster left gauge");

    // Initialize SVG renderers
    fuel_icon_svg_renderer_.load(QString(":/mercedes_190e_cluster_gauge/gas_icon.svg"));
    if (!fuel_icon_svg_renderer_.isValid())
    {
        SPDLOG_ERROR("Failed to load the fuel icon SVG.");
    }

    oil_icon_svg_renderer_.load(QString(":/mercedes_190e_cluster_gauge/oil_icon.svg"));
    if (!oil_icon_svg_renderer_.isValid())
    {
        SPDLOG_ERROR("Failed to load the oil icon SVG.");
    }
    coolant_icon_svg_renderer_.load(QString(":/mercedes_190e_cluster_gauge/coolant_icon.svg"));
    if (!coolant_icon_svg_renderer_.isValid())
    {
        SPDLOG_ERROR("Failed to load the coolant icon SVG.");
    }
}

void Mercedes190EClusterGauge::setFuelGaugeValue(float value)
{
    fuel_gauge_current_value_ = std::clamp(value, m_config.fuel_gauge.min_value, m_config.fuel_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setOilPressureGaugeValue(float value)
{
    oil_pressure_gauge_current_value_ = std::clamp(value, m_config.right_gauge.min_value, m_config.right_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setEconomyGaugeValue(float value)
{
    economy_gauge_current_value_ = std::clamp(value, m_config.bottom_gauge.min_value, m_config.bottom_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setCoolantTemperatureGaugeValue(float value)
{
    coolant_temperature_gauge_current_value_ = std::clamp(value, m_config.left_gauge.min_value, m_config.left_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::applyPaintTransform(QPainter& painter) const
{
    gauge_paint::applyCenteredScale(painter, *this, kCanvasLogicalSize);
}

void Mercedes190EClusterGauge::paintDynamic(QPainter& painter)
{
    float subGaugeRadius = kSubGaugeRadius;
    drawFuelGaugeNeedle(&painter, m_config.fuel_gauge, 0.0f, -subGaugeRadius);
    drawOilPressureGaugeNeedle(&painter, m_config.right_gauge, subGaugeRadius, 0.0f);
    drawCoolantTemperatureGaugeNeedle(&painter, m_config.left_gauge, -subGaugeRadius, 0.0f);
    drawEconomyGaugeNeedle(&painter, m_config.bottom_gauge, 0.0f, subGaugeRadius);
}

void Mercedes190EClusterGauge::paintStaticUnderlay(QPainter& painter)
{
    gauge_paint::drawCircularBackground(painter, kMainGaugeRadius);

    float subGaugeRadius = kSubGaugeRadius;
    drawFuelGaugeBase(&painter, m_config.fuel_gauge, 0.0f, -subGaugeRadius);
    drawOilPressureGaugeBase(&painter, m_config.right_gauge, subGaugeRadius, 0.0f);
    drawCoolantTemperatureGaugeBase(&painter, m_config.left_gauge, -subGaugeRadius, 0.0f);
    drawEconomyGaugeBase(&painter, m_config.bottom_gauge, 0.0f, subGaugeRadius);
}

void Mercedes190EClusterGauge::drawFuelGaugeBase(QPainter *painter, const sub_gauge_config_t& gauge,
                                          float centerX, float centerY)
{
    Q_UNUSED(gauge);
    // Ensure text uses widget-selected font family
    QFont baseFont(m_fontFamily);
    painter->setFont(baseFont);

    // Labels arranged from high (right) to low (left)
    const std::vector<const char*> labels = {"1/1", nullptr, "1/2", nullptr, "R"};
    drawGaugeBase(painter, centerX, centerY, GaugeOrientation::Up, 5, true, labels);

    // Draw the triangle symbol on the left side for fuel reserve marker
    painter->save();
    float triangleX = centerX - kFuelTriangleOffsetX;
    float triangleY = centerY - kFuelTriangleOffsetY;
    QPolygonF triangle;
    triangle << QPointF(triangleX - kFuelTriangleSize/2, triangleY - kFuelTriangleSize/2)
             << QPointF(triangleX - kFuelTriangleSize/2, triangleY + kFuelTriangleSize/2)
             << QPointF(triangleX + kFuelTriangleSize/2, triangleY);
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::white);
    painter->drawPolygon(triangle);
    painter->restore();

    painter->save();
    if (fuel_icon_svg_renderer_.isValid())
    {
        float iconX = centerX - kGaugeIconDefaultSize / 2.0f;
        float iconY = centerY + kGaugeIconDefaultOffsetY;
        fuel_icon_svg_renderer_.render(painter, QRectF(iconX, iconY, kGaugeIconDefaultSize, kGaugeIconDefaultSize));
    }
    painter->restore();
}

void Mercedes190EClusterGauge::drawFuelGaugeNeedle(QPainter *painter, const sub_gauge_config_t& gauge,
                             float centerX, float centerY)
{
    drawGaugeNeedle(painter, gauge, centerX, centerY, GaugeOrientation::Up, fuel_gauge_current_value_);
}

void Mercedes190EClusterGauge::drawOilPressureGaugeBase(QPainter *painter, const sub_gauge_config_t& gauge,
                              float centerX, float centerY)
{
    Q_UNUSED(gauge);
    QFont baseFont(m_fontFamily);
    painter->setFont(baseFont);

    const std::vector<const char*> labels = {"3", "2", "1", "0"};
    drawGaugeBase(painter, centerX, centerY, GaugeOrientation::Right, 4, false, labels);

    painter->save();
    if (oil_icon_svg_renderer_.isValid())
    {
        float iconX = centerX - kOilIconOffsetXFromCenter;
        float iconY = centerY + kOilIconOffsetY;
        oil_icon_svg_renderer_.render(painter, QRectF(iconX, iconY, kOilIconWidth, kOilIconHeight));
    }
    painter->restore();
}

void Mercedes190EClusterGauge::drawOilPressureGaugeNeedle(QPainter *painter, const sub_gauge_config_t& gauge,
                                    float centerX, float centerY)
{
    drawGaugeNeedle(painter, gauge, centerX, centerY, GaugeOrientation::Right, oil_pressure_gauge_current_value_);
}

void Mercedes190EClusterGauge::drawCoolantTemperatureGaugeBase(QPainter *painter, const sub_gauge_config_t& gauge,
                                     float centerX, float centerY)
{
    Q_UNUSED(gauge);
    QFont baseFont(m_fontFamily);
    painter->setFont(baseFont);

    const std::vector<const char*> labels = {"40", nullptr, "80", nullptr, "120"};
    constexpr const char* unitText = "°C";
    const auto info = getOrientationInfo(GaugeOrientation::Left);
    const float gaugeStartAngle = info.orientationAngleDeg - (kGaugeSpanDegrees / 2.0f);

    // Optional unit (e.g., °C)
    painter->save();
    QFont unitFont(painter->font()); unitFont.setPointSizeF(kUnitFontPt);
    painter->setFont(unitFont);
    float unitAngle = gaugeStartAngle + kGaugeSpanDegrees;
    float unitAngleRad = degrees_to_radians(unitAngle);
    float projectedUnitAngle = std::atan2(centerY + info.projectionSign * kNeedleLength * std::sin(unitAngleRad),
                                            centerX + info.projectionSign * kNeedleLength * std::cos(unitAngleRad));
    QPointF unitPos(kLabelRadius * std::cos(projectedUnitAngle) + kCoolantUnitOffsetX,
                    kLabelRadius * std::sin(projectedUnitAngle) + kCoolantUnitOffsetY);
    painter->drawText(unitPos, unitText);
    painter->restore();

    drawGaugeBase(painter, centerX, centerY, GaugeOrientation::Left, 5, true, labels);

    painter->save();
    if (coolant_icon_svg_renderer_.isValid())
    {
        float iconX = centerX - (kGaugeIconDefaultSize / 2.0f);
        float iconY = centerY + kGaugeIconDefaultOffsetY;
        coolant_icon_svg_renderer_.render(painter, QRectF(iconX, iconY, kGaugeIconDefaultSize, kGaugeIconDefaultSize));
    }
    painter->restore();
}

void Mercedes190EClusterGauge::drawCoolantTemperatureGaugeNeedle(QPainter *painter, const sub_gauge_config_t& gauge,
                                           float centerX, float centerY)
{
    drawGaugeNeedle(painter, gauge, centerX, centerY, GaugeOrientation::Left, coolant_temperature_gauge_current_value_);
}

void Mercedes190EClusterGauge::drawEconomyGaugeBase(QPainter *painter, const sub_gauge_config_t& gauge,
                                                    float centerX, float centerY)
{
    Q_UNUSED(gauge);
    QFont baseFont(m_fontFamily);
    painter->setFont(baseFont);

    // The 190E economy gauge has an unlabeled scale from economical (left) to
    // uneconomical (right); ticks only.
    const std::vector<const char*> labels = {};
    drawGaugeBase(painter, centerX, centerY, GaugeOrientation::Down, 5, true, labels);
}

void Mercedes190EClusterGauge::drawEconomyGaugeNeedle(QPainter *painter, const sub_gauge_config_t& gauge,
                                                      float centerX, float centerY)
{
    drawGaugeNeedle(painter, gauge, centerX, centerY, GaugeOrientation::Down, economy_gauge_current_value_);
}

#include "mercedes_190e_cluster_gauge/moc_mercedes_190e_cluster_gauge.cpp"
