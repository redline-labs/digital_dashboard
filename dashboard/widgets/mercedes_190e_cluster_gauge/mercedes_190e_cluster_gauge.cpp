#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include <QPaintEvent>
#include <QFontMetrics>
#include <QDebug>
#include <QSvgRenderer>
#include <QMetaObject>

#include <spdlog/spdlog.h>

// Expression parser
#include "pub_sub/zenoh_subscriber.h"
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
            info.orientationAngleDeg = 270.0f;
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

    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -kNeedleBaseWidth / 2.0f)
                  << QPointF(kNeedleLength, -kNeedleTipWidth / 2.0f)
                  << QPointF(kNeedleLength,  kNeedleTipWidth / 2.0f)
                  << QPointF(0.0f,  kNeedleBaseWidth / 2.0f);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kNeedleColor);
    painter->drawPolygon(needlePolygon);
    painter->restore();

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(40, 40, 40));
    painter->drawEllipse(QPointF(centerX, centerY), kPivotRadius, kPivotRadius);
    painter->restore();
}

Mercedes190EClusterGauge::Mercedes190EClusterGauge(const Mercedes190EClusterGaugeConfig_t& cfg, QWidget *parent):
  QWidget(parent),
  m_config(cfg),
  fuel_gauge_current_value_(0.0f),
  oil_pressure_gauge_current_value_(0.0f),
  coolant_temperature_gauge_current_value_(0.0f),
  economy_gauge_current_value_(0.0f)
{
    // Load font from Qt resources
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
    if (fontId != -1) {
        m_fontFamily = QFontDatabase::applicationFontFamilies(fontId).at(0);
    } else {
        qWarning("Failed to load futura.ttf from resources. Using default sans-serif.");
        m_fontFamily = "sans-serif";
    }
    
    // Initialize expression parsers for each sub-gauge
    auto initializeSubGaugeParser = [](const sub_gauge_config_t& gauge_config,
                                          std::unique_ptr<pub_sub::ZenohExpressionSubscriber>& parser,
                                          const char* gauge_name)
    {
        try {
            parser = std::make_unique<pub_sub::ZenohExpressionSubscriber>(
                gauge_config.schema_type,
                gauge_config.value_expression,
                gauge_config.zenoh_key
            );
            
            if (!parser->isValid()) {
                SPDLOG_ERROR("Invalid {} gauge expression '{}' for schema '{}' in cluster gauge", 
                            gauge_name, gauge_config.value_expression, reflection::enum_traits<pub_sub::schema_type_t>::to_string(gauge_config.schema_type));
                parser.reset();
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to initialize {} gauge expression parser: {}", gauge_name, e.what());
            parser.reset();
        }
    };
    
    // Initialize parsers for all sub-gauges
    initializeSubGaugeParser(m_config.fuel_gauge, top_gauge_expression_parser_, "top");
    initializeSubGaugeParser(m_config.right_gauge, right_gauge_expression_parser_, "right");
    initializeSubGaugeParser(m_config.bottom_gauge, bottom_gauge_expression_parser_, "bottom");
    initializeSubGaugeParser(m_config.left_gauge, left_gauge_expression_parser_, "left");

    if (top_gauge_expression_parser_) {
        top_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, [this, v]() { setFuelGaugeValue(v); }, Qt::QueuedConnection);
        });
    }

    if (right_gauge_expression_parser_)
    {
        right_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, [this, v]() { setOilPressureGaugeValue(v); }, Qt::QueuedConnection);
        });
    }

    if (bottom_gauge_expression_parser_)
    {
        bottom_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, [this, v](){ setEconomyGaugeValue(v); }, Qt::QueuedConnection);
        });
    }

    if (left_gauge_expression_parser_)
    {
        left_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, [this, v]() { setCoolantTemperatureGaugeValue(v); }, Qt::QueuedConnection);
        });
    }

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

float Mercedes190EClusterGauge::valueToAngle(float value, float minVal, float maxVal)
{
    if (maxVal == minVal)
    {
        return 0.0f;
    }

    float constrainedValue = std::clamp(value, minVal, maxVal);
    float factor = (constrainedValue - minVal) / (maxVal - minVal);

    return factor * kGaugeSpanDegrees;
}

void Mercedes190EClusterGauge::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (static_cache_.size() != size()) {
        updateStaticCache();
    }

    if (!static_cache_.isNull()) {
        painter.drawPixmap(0, 0, static_cache_);
    }

    applyGaugeTransform(&painter);

    float subGaugeRadius = kSubGaugeRadius;
    drawFuelGaugeNeedle(&painter, m_config.fuel_gauge, 0.0f, -subGaugeRadius);
    drawOilPressureGaugeNeedle(&painter, m_config.right_gauge, subGaugeRadius, 0.0f);
    drawCoolantTemperatureGaugeNeedle(&painter, m_config.left_gauge, -subGaugeRadius, 0.0f);
}

void Mercedes190EClusterGauge::applyGaugeTransform(QPainter *painter) const
{
    int side = std::min(width(), height());
    painter->translate(width() / 2.0, height() / 2.0);
    painter->scale(side / kCanvasLogicalSize, side / kCanvasLogicalSize);
}

void Mercedes190EClusterGauge::updateStaticCache()
{
    if (width() <= 0 || height() <= 0) {
        static_cache_ = QPixmap();
        return;
    }

    static_cache_ = QPixmap(size());
    static_cache_.fill(Qt::transparent);

    QPainter cachePainter(&static_cache_);
    cachePainter.setRenderHint(QPainter::Antialiasing);
    applyGaugeTransform(&cachePainter);

    drawBackground(&cachePainter);

    float subGaugeRadius = kSubGaugeRadius;
    drawFuelGaugeBase(&cachePainter, m_config.fuel_gauge, 0.0f, -subGaugeRadius);
    drawOilPressureGaugeBase(&cachePainter, m_config.right_gauge, subGaugeRadius, 0.0f);
    drawCoolantTemperatureGaugeBase(&cachePainter, m_config.left_gauge, -subGaugeRadius, 0.0f);
}

void Mercedes190EClusterGauge::drawBackground(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawEllipse(QPointF(0.0f, 0.0f), kMainGaugeRadius, kMainGaugeRadius); // Main background circle
    painter->restore();
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

#include "mercedes_190e_cluster_gauge/moc_mercedes_190e_cluster_gauge.cpp"
