#include "sparkline/sparkline.h"
#include <QHBoxLayout>
#include <QFont>
#include <QPen>
#include <QPainterPath>
#include <QLinearGradient>
#include <QBrush>
#include <QTimer>
#include <QtGlobal> // For qBound if needed, or std::clamp in C++17

SparklineItem::SparklineItem(const QString &units, QWidget *parent)
    : QWidget(parent), units(units), m_fixedMinY(0.0), m_fixedMaxY(100.0) { // Initialize fixed range
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    
    // Value and Units display
    QWidget *valueUnitsWidget = new QWidget(this);
    QVBoxLayout *valueUnitsLayout = new QVBoxLayout(valueUnitsWidget);
    valueUnitsLayout->setSpacing(0);
    valueUnitsLayout->setContentsMargins(0,0,0,0);

    valueLabel = new QLabel("0", this);
    QFont valueFont = valueLabel->font();
    valueFont.setPointSize(24); // Large font for value
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    unitsLabel = new QLabel(units, this);
    QFont unitsFont = unitsLabel->font();
    unitsFont.setPointSize(10); // Smaller font for units
    unitsLabel->setFont(unitsFont);
    unitsLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);

    valueUnitsLayout->addWidget(valueLabel);
    valueUnitsLayout->addWidget(unitsLabel);
    valueUnitsLayout->addStretch();
    valueUnitsWidget->setLayout(valueUnitsLayout);
    valueUnitsWidget->setFixedWidth(80);

    mainLayout->addStretch(); // This will be the sparkline area
    mainLayout->addWidget(valueUnitsWidget);
    setLayout(mainLayout);

    setMinimumHeight(60);
    // Add some initial data for visual testing
    // for(int i = 0; i < 20; ++i) {
    //     addDataPoint(qrand() % 100);
    // }

    // Initialize dataPoints with default values to ensure it's always full for scrolling
    for (int i = 0; i < MAX_DATA_POINTS; ++i) {
        dataPoints.append(0.0); // Initialize with a baseline value, e.g., 0
    }
    m_lastValue = 0.0; // Keep track of the last real value

    // Initialize and start the repaint timer
    m_repaintTimer = new QTimer(this);
    connect(m_repaintTimer, &QTimer::timeout, this, &SparklineItem::forceRepaint);
    m_repaintTimer->start(1000 / 30); // 30 Hz, approx 33ms interval
}

void SparklineItem::setYAxisRange(double minVal, double maxVal) {
    if (minVal < maxVal) {
        m_fixedMinY = minVal;
        m_fixedMaxY = maxVal;
    } else {
        // Handle error or set to a default valid range
        m_fixedMinY = 0.0;
        m_fixedMaxY = 100.0;
    }
    // No need to call update() here, as paintEvent will use these values on next repaint
}

void SparklineItem::addDataPoint(double value) {
    m_lastValue = value; 
    // The actual data points are shifted in forceRepaint.
    // We don't directly modify dataPoints here anymore based on latest user changes.
}

void SparklineItem::forceRepaint() {
    // This method is called by the timer at 30Hz
    // To simulate continuous scrolling, we shift the data
    if (!dataPoints.isEmpty()) {
        dataPoints.removeFirst();
        // When adding m_lastValue, we might want to clamp it to the fixed Y range
        // if strict adherence to the range is required for new points.
        // double valueToAppend = qBound(m_fixedMinY, m_lastValue, m_fixedMaxY);
        // dataPoints.append(valueToAppend);
        dataPoints.append(m_lastValue); // Using unclamped m_lastValue for now
    }
    // Ensure dataPoints always has MAX_DATA_POINTS after manipulation
    // This should ideally not be needed if logic is correct but acts as a safeguard.
    while(dataPoints.size() < MAX_DATA_POINTS && MAX_DATA_POINTS > 0) {
        dataPoints.prepend(0.0f); // Pad with last known value if it got too short
    }
    while(dataPoints.size() > MAX_DATA_POINTS) {
        dataPoints.removeFirst(); // Trim if it got too long
    }

    valueLabel->setText(QString::number(m_lastValue, 'f', 1)); // Display value with 1 decimal place

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
    double minVal = m_fixedMinY;
    double maxVal = m_fixedMaxY;

    // Ensure maxVal is not equal to minVal to prevent division by zero
    // This check is critical if m_fixedMinY can be equal to m_fixedMaxY
    if (maxVal == minVal) {
        maxVal += 1.0; // Or handle as an error / no plot
    }

    QColor lineColor = Qt::blue;
    QColor gradientStartColor = lineColor.darker(120); // Slightly darker blue
    QColor gradientEndColor = gradientStartColor;
    gradientEndColor.setAlpha(0x00); // Semi-transparent blue for the bottom of the gradient

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

    float currentX = graphRect.left();
    // Clamp the y-value calculation to the graphRect boundaries
    // This prevents drawing outside the box if data points exceed m_fixedMinY/m_fixedMaxY
    float firstDataY = dataPoints[0];
    float firstNormY = (firstDataY - minVal) / yRange;
    // float firstClampedNormY = qBound(0.0f, static_cast<float>(firstNormY), 1.0f);
    // float currentY = graphRect.bottom() - (firstClampedNormY * graphRect.height());
    float currentY = graphRect.bottom() - (static_cast<float>(firstNormY) * graphRect.height());
    linePath.moveTo(currentX, currentY);

    for (int i = 1; i < pointsToDraw; ++i) {
        currentX += xStep;
        float dataY = dataPoints[i];
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
    gradient.setColorAt(0, gradientStartColor); // Start color (top)
    gradient.setColorAt(1, gradientEndColor);   // End color (bottom)

    // Fill the area under the line
    painter.fillPath(fillPath, QBrush(gradient));

    // Draw the line itself on top
    QPen pen(lineColor);
    pen.setWidth(2);
    painter.setPen(pen);
    painter.drawPath(linePath);
} 

#include "sparkline/moc_sparkline.cpp"
