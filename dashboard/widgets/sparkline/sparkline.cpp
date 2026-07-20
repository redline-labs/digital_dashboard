#include "sparkline/sparkline.h"
#include <QHBoxLayout>
#include <QFont>
#include <QPen>
#include <QPainterPath>
#include <QLinearGradient>
#include <QBrush>
#include <QTimer>
#include <QtGlobal> // For qBound if needed, or std::clamp in C++17
#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include <algorithm>
#include <memory>

SparklineItem::SparklineItem(const SparklineConfig_t& cfg, QWidget *parent)
    : QWidget(parent), _cfg{cfg}
{
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    
    // Value and Units display
    QWidget *valueUnitsWidget = new QWidget(this);
    QVBoxLayout *valueUnitsLayout = new QVBoxLayout(valueUnitsWidget);
    valueUnitsLayout->setSpacing(0);
    valueUnitsLayout->setContentsMargins(0,0,0,0);

    valueLabel = new QLabel("0", this);
    QFont valueFont(_cfg.font_family.c_str());
    valueFont.setPointSize(_cfg.font_size_value);
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    
    // Apply text color from configuration
    valueLabel->setStyleSheet(QString("color: %1;").arg(_cfg.text_color.c_str()));

    unitsLabel = new QLabel(_cfg.units.c_str(), this);
    QFont unitsFont(_cfg.font_family.c_str());
    unitsFont.setPointSize(_cfg.font_size_units);
    unitsLabel->setFont(unitsFont);
    unitsLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    
    // Apply text color from configuration
    unitsLabel->setStyleSheet(QString("color: %1;").arg(_cfg.text_color.c_str()));

    valueUnitsLayout->addWidget(valueLabel);
    valueUnitsLayout->addWidget(unitsLabel);
    valueUnitsLayout->addStretch();
    valueUnitsWidget->setLayout(valueUnitsLayout);
    valueUnitsWidget->setFixedWidth(80);

    mainLayout->addStretch(); // This will be the sparkline area
    mainLayout->addWidget(valueUnitsWidget);
    setLayout(mainLayout);

    setMinimumHeight(60);

    // Initialize dataPoints with default values to ensure it's always full for scrolling
    dataPoints.resize(MAX_DATA_POINTS);
    dataPoints.fill(0.0);
    m_lastValue = 0.0; // Keep track of the last real value
    m_writeIndex = 0;

    // Build paint resources once; they only depend on config.
    m_lineColor = QColor::fromString(_cfg.line_color.c_str());
    m_gradientStartColor = m_lineColor.darker(120);
    m_gradientEndColor = m_gradientStartColor;
    m_gradientEndColor.setAlpha(0x00); // Fade to transparent at the bottom
    m_linePen = QPen(m_lineColor);
    m_linePen.setWidth(2);


    _expression_parser = dashboard::makeExpressionSubscription<double>(
        _cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
        this, &SparklineItem::setLatestValue, "sparkline value");

    // Initialize and start the repaint timer
    m_repaintTimer = new QTimer(this);
    connect(m_repaintTimer, &QTimer::timeout, this, &SparklineItem::forceRepaint);
    m_repaintTimer->start(1000 / std::max<uint16_t>(1, _cfg.update_rate));
}

void SparklineItem::setYAxisRange(double minVal, double maxVal) {
    if (minVal < maxVal) {
        _cfg.min_value = minVal;
        _cfg.max_value = maxVal;
    } else {
        // Handle error or set to a default valid range
        _cfg.min_value = 0.0;
        _cfg.max_value = 100.0;
    }
    // No need to call update() here, as paintEvent will use these values on next repaint
}

void SparklineItem::setLatestValue(double value) {
    m_lastValue = value;
    // The actual data points are shifted in forceRepaint.
}

void SparklineItem::forceRepaint() {
    // This method is called by the timer at 30Hz
    // To simulate continuous scrolling, we shift the data
    if (!dataPoints.isEmpty()) {
        dataPoints[m_writeIndex] = m_lastValue;
        m_writeIndex = (m_writeIndex + 1) % dataPoints.size();
    }

    // Only touch the QLabel (relayout/repaint) when the displayed text changes.
    QString valueText = QString::number(m_lastValue, 'f', 1);
    if (valueText != m_lastValueText)
    {
        m_lastValueText = valueText;
        valueLabel->setText(valueText);
    }

    update(); // Schedule a repaint
}

void SparklineItem::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Added a check to prevent crash if labels are somehow null during early init or destruction
    if (!valueLabel || !unitsLabel) { 
        return;
    }
    
    // Ensure dataPoints is not empty before trying to access its elements for min/max or drawing
    if (dataPoints.isEmpty()) {
        return;
    }

    int textBlockHeight = valueLabel->height() + unitsLabel->height();
    if (textBlockHeight < 10) { 
        textBlockHeight = 10;
    }

    int padding = 5;
    int graphPlotWidth = width() - 80 - (2 * padding); 
    if (graphPlotWidth < 0) {
        graphPlotWidth = 0;
    }
    QRectF graphRect(padding, padding, graphPlotWidth, textBlockHeight);

    // Use fixed Y-axis range
    double minVal = _cfg.min_value;
    double maxVal = _cfg.max_value;

    // Ensure maxVal is not equal to minVal to prevent division by zero
    // This check is critical if _cfg.min_value can be equal to _cfg.max_value
    if (maxVal == minVal) {
        maxVal += 1.0; // Or handle as an error / no plot
    }

    QPainterPath linePath;
    // Ensure MAX_DATA_POINTS is greater than 1 to avoid division by zero if graphRect.width() is > 0
    // And also that graphRect.width() > 0
    float xStep = (MAX_DATA_POINTS > 1 && graphRect.width() > 0) ? 
                  (static_cast<float>(graphRect.width()) / (MAX_DATA_POINTS - 1)) : 
                  0.0f;

    float yRange = maxVal - minVal;
    // yRange should not be zero here due to the check above

    // dataPoints should always contain MAX_DATA_POINTS elements now
    int pointsToDraw = dataPoints.size(); // Should be MAX_DATA_POINTS
    if (pointsToDraw < 2) return; 
    const auto getPoint = [&](int i) -> double {
        const int idx = (m_writeIndex + i) % dataPoints.size();
        return dataPoints[idx];
    };

    float currentX = graphRect.left();
    // Clamp the y-value calculation to the graphRect boundaries
    // This prevents drawing outside the box if data points exceed _cfg.min_value/_cfg.max_value
    float firstDataY = static_cast<float>(getPoint(0));
    float firstNormY = (firstDataY - minVal) / yRange;
    // float firstClampedNormY = qBound(0.0f, static_cast<float>(firstNormY), 1.0f);
    // float currentY = graphRect.bottom() - (firstClampedNormY * graphRect.height());
    float currentY = graphRect.bottom() - (static_cast<float>(firstNormY) * graphRect.height());
    linePath.moveTo(currentX, currentY);

    for (int i = 1; i < pointsToDraw; ++i) {
        currentX += xStep;
        float dataY = static_cast<float>(getPoint(i));
        float normY = (dataY - minVal) / yRange;
        // float clampedNormY = qBound(0.0f, static_cast<float>(normY), 1.0f);
        // float yPos = graphRect.bottom() - (clampedNormY * graphRect.height());
        float yPos = graphRect.bottom() - (static_cast<float>(normY) * graphRect.height());
        linePath.lineTo(currentX, yPos);
    }

    // Create the fill path
    QPainterPath fillPath = linePath;
    // Get the last point of the line path
    QPointF lastDataPoint = linePath.elementAt(linePath.elementCount() -1 );
    // Line down to the bottom of the graph rectangle
    fillPath.lineTo(lastDataPoint.x(), graphRect.bottom());
    // Line to the bottom-left of the graph rectangle (aligned with the first data point's x)
    QPointF firstDataPoint = linePath.elementAt(0);
    fillPath.lineTo(firstDataPoint.x(), graphRect.bottom());
    // Close the path back to the first data point (implicitly done by QPainter::fillPath with a brush if not explicitly closed)
    fillPath.lineTo(firstDataPoint.x(), firstDataPoint.y()); // Explicitly close for clarity

    // Define the gradient
    QLinearGradient gradient(graphRect.topLeft(), graphRect.bottomLeft());
    gradient.setColorAt(0, m_gradientStartColor); // Start color (top)
    gradient.setColorAt(1, m_gradientEndColor);   // End color (bottom)

    // Fill the area under the line
    painter.fillPath(fillPath, QBrush(gradient));

    // Draw the line itself on top
    painter.setPen(m_linePen);
    painter.drawPath(linePath);
}

#include "sparkline/moc_sparkline.cpp"
