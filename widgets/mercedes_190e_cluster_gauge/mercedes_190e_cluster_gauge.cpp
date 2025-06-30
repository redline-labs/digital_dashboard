#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include <QPaintEvent>
#include <QFontMetrics>
#include <QDebug>

#include <spdlog/spdlog.h>

#include <cmath>
#include <numbers>

// Helper for degree to radian conversion
constexpr float degreesToRadians(float degrees)
{
    return degrees * (std::numbers::pi_v<float> / 180.0f);
}

Mercedes190EClusterGauge::Mercedes190EClusterGauge(const cluster_gauge_config_t& cfg, QWidget *parent)
    : QWidget(parent), m_config(cfg)
{
    // Load font from Qt resources
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
    if (fontId != -1) {
        m_fontFamily = QFontDatabase::applicationFontFamilies(fontId).at(0);
    } else {
        qWarning("Failed to load futura.ttf from resources. Using default sans-serif.");
        m_fontFamily = "sans-serif";
    }
}

void Mercedes190EClusterGauge::setTopGaugeValue(float value)
{
    m_config.top_gauge.current_value = qBound(m_config.top_gauge.min_value, value, m_config.top_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setRightGaugeValue(float value)
{
    m_config.right_gauge.current_value = qBound(m_config.right_gauge.min_value, value, m_config.right_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setBottomGaugeValue(float value)
{
    m_config.bottom_gauge.current_value = qBound(m_config.bottom_gauge.min_value, value, m_config.bottom_gauge.max_value);
    update();
}

void Mercedes190EClusterGauge::setLeftGaugeValue(float value)
{
    m_config.left_gauge.current_value = qBound(m_config.left_gauge.min_value, value, m_config.left_gauge.max_value);
    update();
}

float Mercedes190EClusterGauge::getTopGaugeValue() const
{
    return m_config.top_gauge.current_value;
}

float Mercedes190EClusterGauge::getRightGaugeValue() const
{
    return m_config.right_gauge.current_value;
}

float Mercedes190EClusterGauge::getBottomGaugeValue() const
{
    return m_config.bottom_gauge.current_value;
}

float Mercedes190EClusterGauge::getLeftGaugeValue() const
{
    return m_config.left_gauge.current_value;
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
    drawSubGauge(&painter, m_config.top_gauge, 0.0f, -subGaugeRadius, 0.0f);  // Start at top

    // Right gauge (3 o'clock)
    //drawSubGauge(&painter, m_config.right_gauge, subGaugeRadius, 0.0f, 0.0f);   // Start at right

    // Bottom gauge (6 o'clock)
    //drawSubGauge(&painter, m_config.bottom_gauge, 0.0f, subGaugeRadius, 90.0f);  // Start at bottom

    // Left gauge (9 o'clock)
    //drawSubGauge(&painter, m_config.left_gauge, -subGaugeRadius, 0.0f, 180.0f); // Start at left
}

void Mercedes190EClusterGauge::drawBackground(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawEllipse(QPointF(0.0f, 0.0f), 100.0f, 100.0f); // Main background circle
    painter->restore();
}

void Mercedes190EClusterGauge::drawSubGauge(QPainter *painter, const cluster_gauge_config_t::sub_gauge_config_t& gauge,
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
    float current_value = 20.0f; // gauge.current_value
    float valueRatio = (current_value - gauge.min_value) / (gauge.max_value - gauge.min_value);
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

#include "mercedes_190e_cluster_gauge/moc_mercedes_190e_cluster_gauge.cpp"
