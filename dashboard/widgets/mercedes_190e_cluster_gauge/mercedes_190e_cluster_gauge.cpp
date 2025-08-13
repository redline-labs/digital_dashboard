#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include <QPaintEvent>
#include <QFontMetrics>
#include <QDebug>
#include <QSvgRenderer>
#include <QMetaObject>

#include <spdlog/spdlog.h>

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>

// Expression parser
#include "expression_parser/expression_parser.h"

#include <cmath>
#include <numbers>

// Helper for degree to radian conversion
constexpr float degreesToRadians(float degrees)
{
    return degrees * (std::numbers::pi_v<float> / 180.0f);
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
    auto initializeSubGaugeParser = [this](const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge_config,
                                          std::unique_ptr<expression_parser::ExpressionParser>& parser,
                                          const char* gauge_name) {
        if (!gauge_config.zenoh_key.empty() && !gauge_config.schema_type.empty() && !gauge_config.value_expression.empty()) {
            try {
                parser = std::make_unique<expression_parser::ExpressionParser>(
                    gauge_config.schema_type,
                    gauge_config.value_expression,
                    gauge_config.zenoh_key
                );
                
                if (!parser->isValid()) {
                    SPDLOG_ERROR("Invalid {} gauge expression '{}' for schema '{}' in cluster gauge", 
                                gauge_name, gauge_config.value_expression, gauge_config.schema_type);
                    parser.reset();
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Failed to initialize {} gauge expression parser: {}", gauge_name, e.what());
                parser.reset();
            }
        }
    };
    
    // Initialize parsers for all sub-gauges
    initializeSubGaugeParser(m_config.fuel_gauge, top_gauge_expression_parser_, "top");
    initializeSubGaugeParser(m_config.right_gauge, right_gauge_expression_parser_, "right");
    initializeSubGaugeParser(m_config.bottom_gauge, bottom_gauge_expression_parser_, "bottom");
    initializeSubGaugeParser(m_config.left_gauge, left_gauge_expression_parser_, "left");

    if (top_gauge_expression_parser_) {
        top_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, "onTopGaugeEvaluated", Qt::QueuedConnection, Q_ARG(float, v));
        });
    }
    if (right_gauge_expression_parser_) {
        right_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, "onRightGaugeEvaluated", Qt::QueuedConnection, Q_ARG(float, v));
        });
    }
    if (bottom_gauge_expression_parser_) {
        bottom_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, "onBottomGaugeEvaluated", Qt::QueuedConnection, Q_ARG(float, v));
        });
    }
    if (left_gauge_expression_parser_) {
        left_gauge_expression_parser_->setResultCallback<float>([this](float v) {
            QMetaObject::invokeMethod(this, "onLeftGaugeEvaluated", Qt::QueuedConnection, Q_ARG(float, v));
        });
    }
}

void Mercedes190EClusterGauge::setTopGaugeValue(float value)
{
    fuel_gauge_current_value_ = qBound(m_config.fuel_gauge.min_value, value, m_config.fuel_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setRightGaugeValue(float value)
{
    oil_pressure_gauge_current_value_ = qBound(m_config.right_gauge.min_value, value, m_config.right_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setBottomGaugeValue(float value)
{
    economy_gauge_current_value_ = qBound(m_config.bottom_gauge.min_value, value, m_config.bottom_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setLeftGaugeValue(float value)
{
    coolant_temperature_gauge_current_value_ = qBound(m_config.left_gauge.min_value, value, m_config.left_gauge.max_value);
    update();
}

float Mercedes190EClusterGauge::getTopGaugeValue() const
{
    return fuel_gauge_current_value_;
}

float Mercedes190EClusterGauge::getRightGaugeValue() const
{
    return oil_pressure_gauge_current_value_;
}

float Mercedes190EClusterGauge::getBottomGaugeValue() const
{
    return economy_gauge_current_value_;
}

float Mercedes190EClusterGauge::getLeftGaugeValue() const
{
    return coolant_temperature_gauge_current_value_;
}

float Mercedes190EClusterGauge::valueToAngle(float value, float minVal, float maxVal)
{
    if (maxVal == minVal) return 0.0f;

    float constrainedValue = qBound(minVal, value, maxVal);
    float factor = (constrainedValue - minVal) / (maxVal - minVal);

    // Each sub-gauge spans approximately 90 degrees
    float angleSpan = 90.0f;
    return factor * angleSpan;
}

void Mercedes190EClusterGauge::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int side = qMin(width(), height());
    painter.translate(width() / 2.0, height() / 2.0); // Origin to center
    painter.scale(side / 200.0, side / 200.0); // Logical 200x200 unit square

    drawBackground(&painter);

    // Draw the 4 sub-gauges at their respective positions
    // Each sub-gauge is positioned at a radius from the center
    float subGaugeRadius = 45.0f;

    // Top gauge (12 o'clock)
    drawSubGauge(&painter, m_config.fuel_gauge, 0.0f, -subGaugeRadius, 0.0f);  // Start at top

    // Right gauge (3 o'clock) - Oil Pressure
    drawOilPressureGauge(&painter, m_config.right_gauge, subGaugeRadius, 0.0f);

    // Bottom gauge (6 o'clock)
    //drawSubGauge(&painter, m_config.bottom_gauge, 0.0f, subGaugeRadius, 90.0f);  // Start at bottom

    // Left gauge (9 o'clock) - Coolant Temperature
    drawCoolantTemperatureGauge(&painter, m_config.left_gauge, -subGaugeRadius, 0.0f);
}

void Mercedes190EClusterGauge::drawBackground(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawEllipse(QPointF(0.0f, 0.0f), 100.0f, 100.0f); // Main background circle
    painter->restore();
}

void Mercedes190EClusterGauge::drawSubGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge,
                                          float centerX, float centerY, float startAngle)
{
    painter->save();

    //const float subGaugeRadius = 25.0f;
    const float tickRadius = 98.0f;
    //const float gaugeArcThickness = 1.5f;

     // Needle properties (Orange, tapered) - scaled down for sub-gauge
    const QColor needleColor(255, 165, 0); // Orange like speedometer
    const float needleBaseWidth = 3.0f;    // Width at the pivot (scaled down)
    const float needleTipWidth = 2.0f;     // Width at the tip
    const float needleLength = 50.0f;

    // Calculate the arc range for this sub-gauge position
    // Each gauge arc should be on the "outer" side and span 90 degrees opening toward center
    // The arc should be positioned opposite to the radial direction toward center
    const float gaugeSpan = 90.0f; // Sweep 90 degrees clockwise
    
    // Qt coordinate system.  0 degrees is a 3-o'clock position.  Positive values are counter
    // clockwise.
    const float gaugeStartAngle = startAngle - (gaugeSpan / 2.0f) + 90.0f;

    // Draw tick marks
    const int numTicks = 5;
    for (int i = 0; i < numTicks; ++i)
    {
        // Calculate the value ratio for this tick (inverted: 0 = max, 4 = min)
        float valueRatio = static_cast<float>(numTicks - 1 - i) / (numTicks - 1);
        
        // Calculate where the needle would point for this value
        float needleAngleForTick = gaugeStartAngle + ((1.0f - valueRatio) * gaugeSpan);
        float needleAngleRad = degreesToRadians(needleAngleForTick);
        
        // Calculate where the needle tip would be when pointing at this angle from sub-gauge center
        QPointF needleTip(centerX + needleLength * -1.0f * std::cos(needleAngleRad),
                         centerY + needleLength * -1.0f * std::sin(needleAngleRad));
        
        // Calculate the angle from main gauge center (0,0) to the needle tip
        float angleFromMainCenter = std::atan2(needleTip.y(), needleTip.x());
        
        // Position the tick at this angle on the outer radius
        float tickOuterRadius = tickRadius;
        QPointF tickOuter(tickOuterRadius * std::cos(angleFromMainCenter),
                         tickOuterRadius * std::sin(angleFromMainCenter));
        
        // Calculate the direction from the outer tick point to the sub-gauge center
        QPointF dirToSubGaugeCenter = QPointF(centerX, centerY) - tickOuter;
        
        // Normalize the direction vector
        float dirLength = std::sqrt(dirToSubGaugeCenter.x() * dirToSubGaugeCenter.x() + 
                                   dirToSubGaugeCenter.y() * dirToSubGaugeCenter.y());
        if (dirLength > 0.0f) {
            dirToSubGaugeCenter.setX(dirToSubGaugeCenter.x() / dirLength);
            dirToSubGaugeCenter.setY(dirToSubGaugeCenter.y() / dirLength);
        }
        
        // Calculate the inner tick point along the direction toward sub-gauge center
        float tickLength = 8.0f;
        QPointF tickInner = tickOuter + dirToSubGaugeCenter * tickLength;

        float thickness = i % 2 == 0 ? 3.0f : 2.0f;
        QPen tickPen(Qt::white, thickness);
        painter->setPen(tickPen);
        painter->drawLine(tickOuter, tickInner);
    }

    // Draw labels below certain ticks
    painter->save();
    
    // Set up font for labels
    QFont labelFont(m_fontFamily);
    labelFont.setPointSizeF(9.0f);
    painter->setFont(labelFont);
    painter->setPen(Qt::white);
    QFontMetricsF fm(labelFont);
    
    // Label positions: 0 = rightmost (max), 2 = middle, 4 = leftmost (min)
    const char* labels[] = {"R", nullptr, "1/2", nullptr, "1/1"};
    
    for (int i = 0; i < numTicks; ++i)
    {
        if (labels[i] == nullptr) continue;
        
        // Calculate the value ratio for this tick (inverted: 0 = max, 4 = min)
        float valueRatio = static_cast<float>(numTicks - 1 - i) / (numTicks - 1);
        
        // Calculate where the needle would point for this value
        float needleAngleForTick = gaugeStartAngle + ((1.0f - valueRatio) * gaugeSpan);
        float needleAngleRad = degreesToRadians(needleAngleForTick);
        
        // Calculate where the needle tip would be when pointing at this angle from sub-gauge center
        QPointF needleTip(centerX + needleLength * -1.0f * std::cos(needleAngleRad),
                         centerY + needleLength * -1.0f * std::sin(needleAngleRad));
        
        // Calculate the angle from main gauge center (0,0) to the needle tip
        float angleFromMainCenter = std::atan2(needleTip.y(), needleTip.x());
        
        // Position the label below the tick
        float labelRadius = tickRadius - 17.0f; // Position labels inside the ticks
        QPointF labelPos(labelRadius * std::cos(angleFromMainCenter),
                        labelRadius * std::sin(angleFromMainCenter));
        
        // Draw the label
        QString labelText = QString::fromUtf8(labels[i]);
        QRectF textRect = fm.boundingRect(labelText);
        textRect.moveCenter(labelPos);
        painter->drawText(textRect, Qt::AlignCenter, labelText);
    }
    
    // Draw the triangle symbol on the left side
    painter->save();
    
    // Position triangle to the left of the gauge
    float triangleX = centerX - 46.0f;
    float triangleY = centerY - 30.0f;
    float triangleSize = 8.0f;
    
    // Create triangle pointing right
    QPolygonF triangle;
    triangle << QPointF(triangleX - triangleSize/2, triangleY - triangleSize/2)
             << QPointF(triangleX - triangleSize/2, triangleY + triangleSize/2)
             << QPointF(triangleX + triangleSize/2, triangleY);
    
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::white);
    painter->drawPolygon(triangle);
    
    painter->restore();
    painter->restore();

    // Calculate needle angle based on gauge value
    float valueRatio = (fuel_gauge_current_value_ - gauge.min_value) / (gauge.max_value - gauge.min_value);
    valueRatio = qBound(0.0f, valueRatio, 1.0f); // Clamp to 0-1 range

    float needleAngle = gaugeStartAngle + ((1.0f - valueRatio) * gaugeSpan);

    painter->save();
    painter->translate(centerX, centerY); // Move origin to gauge center
    painter->rotate(-1.0f * needleAngle); // Rotate the coordinate system... Positive is CLOCKWISE.
    
    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -needleBaseWidth / 2.0f)  // Bottom-left at pivot
                  << QPointF(needleLength, -needleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(needleLength, needleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, needleBaseWidth / 2.0f);   // Top-left at pivot
    
    painter->setPen(Qt::NoPen); // No border for the needle itself
    painter->setBrush(needleColor);
    painter->drawPolygon(needlePolygon);
    
    painter->restore();
    
    // Draw center pivot (dark grey/black, flat circle) - scaled down for sub-gauge
    drawCenterHole(painter, centerX, centerY);
    
    // Draw gas icon below the pivot
    painter->save();
    
    // Load and render the gas icon SVG
    QSvgRenderer svgRenderer(QString(":/mercedes_190e_cluster_gauge/gas_icon.svg"));
    if (svgRenderer.isValid()) {
        // Position the icon below the pivot
        float iconSize = 20.0f; // Size of the icon
        float iconX = centerX - iconSize / 2.0f;
        float iconY = centerY + 10.0f; // Position below the pivot
        
        // Render the SVG directly - it already has black fill with transparent cutouts
        svgRenderer.render(painter, QRectF(iconX, iconY, iconSize, iconSize));
    }
    
    painter->restore();
    
    painter->restore();
}

void Mercedes190EClusterGauge::drawCenterHole(QPainter *painter, float centerX, float centerY)
{
    painter->save();
    
    // Central pivot (dark grey/black, flat circle) - scaled down for sub-gauge
    const float pivotRadius = 6.0f; // Scaled down from speedometer's 8.0f
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(40, 40, 40)); // Dark grey like speedometer
    painter->drawEllipse(QPointF(centerX, centerY), pivotRadius, pivotRadius);
    
    painter->restore();
}

void Mercedes190EClusterGauge::drawOilPressureGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge,
                              float centerX, float centerY)
{
    painter->save();

    const float tickRadius = 98.0f;

    // Needle properties (Orange, tapered) - scaled down for sub-gauge
    const QColor needleColor(255, 165, 0); // Orange like speedometer
    const float needleBaseWidth = 3.0f;    // Width at the pivot (scaled down)
    const float needleTipWidth = 2.0f;     // Width at the tip
    const float needleLength = 50.0f;

    // Oil pressure gauge is on the right (3 o'clock position)
    // The arc should span 90 degrees from top to bottom on the right side
    const float gaugeSpan = 90.0f;
    const float gaugeStartAngle = -45.0f; // Start at upper right

    // Draw tick marks
    const int numTicks = 4;
    for (int i = 0; i < numTicks; ++i)
    {
        // Calculate the value ratio for this tick
        float valueRatio = static_cast<float>(i) / (numTicks - 1);
        
        // Calculate the angle for this tick mark relative to sub-gauge center
        float tickAngle = gaugeStartAngle + (valueRatio * gaugeSpan);
        float tickAngleRad = degreesToRadians(tickAngle);
        
        // For the right gauge, we need to calculate where this angle projects onto the main gauge edge
        // The sub-gauge center is at (subGaugeRadius, 0), so we offset from there
        float projectedAngle = std::atan2(centerY + 50.0f * std::sin(tickAngleRad), 
                                         centerX + 50.0f * std::cos(tickAngleRad));
        
        // Position the tick on the outer edge of the main gauge
        QPointF tickOuter(tickRadius * std::cos(projectedAngle),
                         tickRadius * std::sin(projectedAngle));
        
        // Calculate the direction from the tick outer point toward the sub-gauge center
        QPointF dirToSubGaugeCenter = QPointF(centerX, centerY) - tickOuter;
        
        // Normalize the direction vector
        float dirLength = std::sqrt(dirToSubGaugeCenter.x() * dirToSubGaugeCenter.x() + 
                                   dirToSubGaugeCenter.y() * dirToSubGaugeCenter.y());
        if (dirLength > 0.0f) {
            dirToSubGaugeCenter.setX(dirToSubGaugeCenter.x() / dirLength);
            dirToSubGaugeCenter.setY(dirToSubGaugeCenter.y() / dirLength);
        }
        
        // Calculate the inner tick point along the direction toward sub-gauge center
        float tickLength = 8.0f;
        QPointF tickInner = tickOuter + dirToSubGaugeCenter * tickLength;

        float thickness = 3.0f;
        QPen tickPen(Qt::white, thickness);
        painter->setPen(tickPen);
        painter->drawLine(tickOuter, tickInner);
    }

    // Draw labels for oil pressure
    painter->save();
    
    // Set up font for labels
    QFont labelFont(m_fontFamily);
    labelFont.setPointSizeF(9.0f);
    painter->setFont(labelFont);
    painter->setPen(Qt::white);
    QFontMetricsF fm(labelFont);
    
    // Label positions for oil pressure: 0, 1, 2, 3
    const char* labels[] = {"3", "2", "1", "0"};
    
    for (int i = 0; i < numTicks; ++i)
    {
        if (labels[i] == nullptr) continue;
        
        // Calculate the value ratio for this tick
        float valueRatio = static_cast<float>(i) / (numTicks - 1);
        
        // Calculate the angle for this label relative to sub-gauge center
        float labelAngle = gaugeStartAngle + (valueRatio * gaugeSpan);
        float labelAngleRad = degreesToRadians(labelAngle);
        
        // For the right gauge, calculate where this angle projects onto the main gauge
        float projectedAngle = std::atan2(centerY + 50.0f * std::sin(labelAngleRad), 
                                         centerX + 50.0f * std::cos(labelAngleRad));
        
        // Position the label inside the ticks
        float labelRadius = 82.0f; // Position labels inside the ticks
        QPointF labelPos(labelRadius * std::cos(projectedAngle),
                        labelRadius * std::sin(projectedAngle));
        
        // Draw the label
        QString labelText = QString::fromUtf8(labels[i]);
        QRectF textRect = fm.boundingRect(labelText);
        textRect.moveCenter(labelPos);
        painter->drawText(textRect, Qt::AlignCenter, labelText);
    }
    
    painter->restore();

    // Calculate needle angle based on gauge value
    float valueRatio = (oil_pressure_gauge_current_value_ - gauge.min_value) / (gauge.max_value - gauge.min_value);
    valueRatio = qBound(0.0f, valueRatio, 1.0f); // Clamp to 0-1 range

    float needleAngle = gaugeStartAngle + (valueRatio * gaugeSpan);

    // Draw the needle
    painter->save();
    painter->translate(centerX, centerY); // Move origin to gauge center
    painter->rotate(-1.0f * needleAngle); // Rotate the coordinate system... Positive is CLOCKWISE.
    
    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -needleBaseWidth / 2.0f)  // Bottom-left at pivot
                  << QPointF(needleLength, -needleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(needleLength, needleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, needleBaseWidth / 2.0f);   // Top-left at pivot
    
    painter->setPen(Qt::NoPen); // No border for the needle itself
    painter->setBrush(needleColor);
    painter->drawPolygon(needlePolygon);
    
    painter->restore();
    
    // Draw center pivot (dark grey/black, flat circle) - scaled down for sub-gauge
    drawCenterHole(painter, centerX, centerY);
    
    // Draw oil icon below the pivot
    painter->save();
    
    // Load and render the oil icon SVG
    QSvgRenderer svgRenderer(QString(":/mercedes_190e_cluster_gauge/oil_icon.svg"));
    if (svgRenderer.isValid()) {
        // Position the icon below the pivot
        float iconHeight = 12.0f;
        float iconWidth = 24.0f;
        float iconX = centerX - 20.0f;
        float iconY = centerY + 12.0f; // Position below the pivot
        
        // Render the SVG directly
        svgRenderer.render(painter, QRectF(iconX, iconY, iconWidth, iconHeight));
    }
    
    painter->restore();
    
    painter->restore();
}

void Mercedes190EClusterGauge::drawCoolantTemperatureGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge,
                                     float centerX, float centerY)
{
    painter->save();

    const float tickRadius = 98.0f;

    // Needle properties (Orange, tapered) - scaled down for sub-gauge
    const QColor needleColor(255, 165, 0); // Orange like speedometer
    const float needleBaseWidth = 3.0f;    // Width at the pivot (scaled down)
    const float needleTipWidth = 2.0f;     // Width at the tip
    const float needleLength = 50.0f;

    // Coolant temperature gauge is on the left (9 o'clock position)
    // The arc should span 90 degrees from bottom to top on the left side
    const float gaugeSpan = 90.0f;
    const float gaugeStartAngle = 135.0f; // Start at lower left

    // Draw tick marks
    const int numTicks = 5;
    for (int i = 0; i < numTicks; ++i)
    {
        // Calculate the value ratio for this tick
        float valueRatio = static_cast<float>(i) / (numTicks - 1);
        
        // Calculate the angle for this tick mark relative to sub-gauge center
        float tickAngle = gaugeStartAngle + (valueRatio * gaugeSpan);
        float tickAngleRad = degreesToRadians(tickAngle);
        
        // For the left gauge, we need to calculate where this angle projects onto the main gauge edge
        float projectedAngle = std::atan2(centerY + 50.0f * std::sin(tickAngleRad), 
                                         centerX + 50.0f * std::cos(tickAngleRad));
        
        // Position the tick on the outer edge of the main gauge
        QPointF tickOuter(tickRadius * std::cos(projectedAngle),
                         tickRadius * std::sin(projectedAngle));
        
        // Calculate the direction from the tick outer point toward the sub-gauge center
        QPointF dirToSubGaugeCenter = QPointF(centerX, centerY) - tickOuter;
        
        // Normalize the direction vector
        float dirLength = std::sqrt(dirToSubGaugeCenter.x() * dirToSubGaugeCenter.x() + 
                                   dirToSubGaugeCenter.y() * dirToSubGaugeCenter.y());
        if (dirLength > 0.0f) {
            dirToSubGaugeCenter.setX(dirToSubGaugeCenter.x() / dirLength);
            dirToSubGaugeCenter.setY(dirToSubGaugeCenter.y() / dirLength);
        }
        
        // Calculate the inner tick point along the direction toward sub-gauge center
        float tickLength = 8.0f;
        QPointF tickInner = tickOuter + dirToSubGaugeCenter * tickLength;

        float thickness = i % 2 == 0 ? 3.0f : 2.0f;
        QPen tickPen(Qt::white, thickness);
        painter->setPen(tickPen);
        painter->drawLine(tickOuter, tickInner);
    }

    // Draw labels for coolant temperature
    painter->save();
    
    // Set up font for labels
    QFont labelFont(m_fontFamily);
    labelFont.setPointSizeF(9.0f);
    painter->setFont(labelFont);
    painter->setPen(Qt::white);
    QFontMetricsF fm(labelFont);
    
    // Label positions for coolant temperature: 40, 80, 120
    const char* labels[] = {"40", nullptr, "80", nullptr, "120"};
    
    for (int i = 0; i < numTicks; ++i)
    {
        if (labels[i] == nullptr) continue;
        
        // Calculate the value ratio for this tick
        float valueRatio = static_cast<float>(i) / (numTicks - 1);
        
        // Calculate the angle for this label relative to sub-gauge center
        float labelAngle = gaugeStartAngle + (valueRatio * gaugeSpan);
        float labelAngleRad = degreesToRadians(labelAngle);
        
        // For the left gauge, calculate where this angle projects onto the main gauge
        float projectedAngle = std::atan2(centerY + 50.0f * std::sin(labelAngleRad), 
                                         centerX + 50.0f * std::cos(labelAngleRad));
        
        // Position the label inside the ticks
        float labelRadius = 82.0f; // Position labels inside the ticks
        QPointF labelPos(labelRadius * std::cos(projectedAngle),
                        labelRadius * std::sin(projectedAngle));
        
        // Draw the label
        QString labelText = QString::fromUtf8(labels[i]);
        QRectF textRect = fm.boundingRect(labelText);
        textRect.moveCenter(labelPos);
        painter->drawText(textRect, Qt::AlignCenter, labelText);
    }
    
    // Draw °C symbol
    painter->save();
    QFont unitFont(m_fontFamily);
    unitFont.setPointSizeF(10.0f);
    painter->setFont(unitFont);
    
    // Position °C to the right of the 120 label
    float unitAngle = gaugeStartAngle + gaugeSpan;
    float unitAngleRad = degreesToRadians(unitAngle);
    float projectedUnitAngle = std::atan2(centerY + 50.0f * std::sin(unitAngleRad), 
                                         centerX + 50.0f * std::cos(unitAngleRad));
    QPointF unitPos(82.0f * std::cos(projectedUnitAngle) - 10.0f,
                   82.0f * std::sin(projectedUnitAngle) - 15.0f);
    painter->drawText(unitPos, "°C");
    painter->restore();
    
    painter->restore();

    // Calculate needle angle based on gauge value
    float valueRatio = (coolant_temperature_gauge_current_value_ - gauge.min_value) / (gauge.max_value - gauge.min_value);
    valueRatio = qBound(0.0f, valueRatio, 1.0f); // Clamp to 0-1 range

    float needleAngle = gaugeStartAngle + (valueRatio * gaugeSpan);

    // Draw the needle
    painter->save();
    painter->translate(centerX, centerY); // Move origin to gauge center
    painter->rotate(needleAngle); // Rotate the coordinate system... Positive is CLOCKWISE.
    
    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -needleBaseWidth / 2.0f)  // Bottom-left at pivot
                  << QPointF(needleLength, -needleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(needleLength, needleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, needleBaseWidth / 2.0f);   // Top-left at pivot
    
    painter->setPen(Qt::NoPen); // No border for the needle itself
    painter->setBrush(needleColor);
    painter->drawPolygon(needlePolygon);
    
    painter->restore();
    
    // Draw center pivot (dark grey/black, flat circle) - scaled down for sub-gauge
    drawCenterHole(painter, centerX, centerY);
    
    // Draw coolant icon below the pivot
    painter->save();
    
    // Load and render the coolant icon SVG
    QSvgRenderer svgRenderer(QString(":/mercedes_190e_cluster_gauge/coolant_icon.svg"));
    if (svgRenderer.isValid()) {
        // Position the icon below the pivot
        float iconSize = 20.0f; // Size of the icon
        float iconX = centerX - iconSize / 2.0f;
        float iconY = centerY + 10.0f; // Position below the pivot
        
        // Render the SVG directly
        svgRenderer.render(painter, QRectF(iconX, iconY, iconSize, iconSize));
    }
    
    painter->restore();
    
    painter->restore();
}

void Mercedes190EClusterGauge::setZenohSession(std::shared_ptr<zenoh::Session> /*session*/)
{
    configureParserSubscriptions();
}

void Mercedes190EClusterGauge::configureParserSubscriptions()
{
}

// Direct widget subscriptions removed; handled by expression_parser

// Data handlers for each sub-gauge
void Mercedes190EClusterGauge::onTopGaugeEvaluated(float value)
{
    setTopGaugeValue(value);
}

void Mercedes190EClusterGauge::onRightGaugeEvaluated(float value)
{
    setRightGaugeValue(value);
}

void Mercedes190EClusterGauge::onBottomGaugeEvaluated(float value)
{
    setBottomGaugeValue(value);
}

void Mercedes190EClusterGauge::onLeftGaugeEvaluated(float value)
{
    setLeftGaugeValue(value);
}

#include "mercedes_190e_cluster_gauge/moc_mercedes_190e_cluster_gauge.cpp"
