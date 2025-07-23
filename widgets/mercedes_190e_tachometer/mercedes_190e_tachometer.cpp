#include <QTime> // Added for current time
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include <QPainter>
#include <QFontDatabase>
#include <QDebug>
#include <cmath> // For std::cos, std::sin
#include <numbers>

// Helper for degree to radian conversion
constexpr float degreesToRadians(float degrees)
{
    return degrees * (std::numbers::pi_v<float> / 180.0f);
}

Mercedes190ETachometer::Mercedes190ETachometer(tachometer_config_t cfg, QWidget *parent)
    : QWidget(parent),
      m_currentRpmValue(0.0f),

      m_angleStart_deg(160.0f),    // 0 RPM at 7 o'clock
      m_angleSweep_deg(220.0f),   // Sweep CCW. End: 160+220=380 -> 20 deg (approx 1-2 o'clock)

      m_scaleRadius(85.0f),      // Outer edge of ticks
      m_numberRadius(62.5f),     // Center of numbers
      m_pivotRadius(7.0f),
      m_needleLength(83.0f),     // From pivot center to needle tip

      // Updated Red Zone for the new image (single block)
      m_redZoneArcWidth(12.0f), // Thickness of the red block
      _cfg{cfg},
      m_currentTime(QTime::currentTime()) // Initialize current time
{
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
    if (fontId == -1) {
        qWarning() << "Mercedes190ETachometer: Failed to load Futura font. Using default.";
        m_fontFamily = QFont().family();
    } else {
        m_fontFamily = QFontDatabase::applicationFontFamilies(fontId).at(0);
    }
    // Adjusted font sizes based on new reference image (numbers are quite large)
    m_dialFont = QFont(m_fontFamily, 12, QFont::Normal);
    m_labelFont = QFont(m_fontFamily, 7, QFont::Normal);

    // Setup timer for clock updates
    if (_cfg.show_clock == true)
    {
        m_clockUpdateTimer = new QTimer(this);
        connect(m_clockUpdateTimer, &QTimer::timeout, this, &Mercedes190ETachometer::updateClockTime);
        m_clockUpdateTimer->start(1000 * 60); // Update every minute
        updateClockTime(); // Initial call to set time
    }
}

void Mercedes190ETachometer::setRpm(float rpm) {
    float clampedRpm = rpm;
    if (clampedRpm < 0.0f) {
        clampedRpm = 0.0f;
    }
    if (clampedRpm > _cfg.max_rpm) {
        clampedRpm = _cfg.max_rpm;
    }

    m_currentRpmValue = clampedRpm;
    update();
}

float Mercedes190ETachometer::getRpm() const {
    return m_currentRpmValue; // Returns direct RPM
}

float Mercedes190ETachometer::valueToAngle(float value) const {
    if (_cfg.max_rpm <= 0.0f) return m_angleStart_deg;
    float proportion = value / _cfg.max_rpm;
    return m_angleStart_deg + proportion * m_angleSweep_deg;
}

void Mercedes190ETachometer::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    int side = qMin(width(), height());
    painter.translate(width() / 2.0f, height() / 2.0f);
    painter.scale(side / 200.0f, side / 200.0f);

    drawBackground(&painter);
    drawRedZone(&painter); // Draw red zone first so ticks can overlay if needed
    drawScaleAndNumbers(&painter);
    drawStaticText(&painter);

    if (_cfg.show_clock == true)
    {
        drawClock(&painter); // Draw clock
    }

    drawNeedle(&painter);
}

void Mercedes190ETachometer::drawBackground(QPainter *painter) {
    painter->save();
    painter->setBrush(Qt::black);
    painter->setPen(Qt::NoPen);
    painter->drawEllipse(QPointF(0,0), 100.0f, 100.0f); 
    painter->restore();
}

void Mercedes190ETachometer::drawScaleAndNumbers(QPainter *painter) {
    painter->save();
    painter->setFont(m_dialFont);
    painter->setPen(Qt::white);
    QFontMetrics fm(m_dialFont);

    // We iterate based on the *displayed* scale (0-70 for labels/major ticks).
    float displayedMax = _cfg.max_rpm / 100.0f; // e.g., 70.0f

    // Revised loop for clarity: Iterate for ticks based on displayed values (0, 1, 2, ..., displayedMax)
    for (float displayed_val_iter = 0; displayed_val_iter <= displayedMax; displayed_val_iter += 5.0f) {
        float actualRpmValue = displayed_val_iter * 100.0f; // Convert displayed value to actual RPM for angle calculation
        float angleDeg = valueToAngle(actualRpmValue);
        float angleRad = degreesToRadians(angleDeg);
        float tickLength;
        QPen currentPen(Qt::white);
        currentPen.setCapStyle(Qt::FlatCap);
        
        // val_for_logic is on the 0-70 scale for determining tick type and number labels
        int val_for_logic = static_cast<int>(std::round(displayed_val_iter));

        bool isMajor = (val_for_logic == 0 || val_for_logic == 5 || (val_for_logic >= 10 && val_for_logic <= displayedMax && val_for_logic % 10 == 0));
        
        tickLength = 12.0f; // Longest
        currentPen.setWidthF(isMajor ? 3.0f : 1.75f);

        painter->setPen(currentPen);
        QPointF p1((m_scaleRadius - tickLength) * std::cos(angleRad), (m_scaleRadius - tickLength) * std::sin(angleRad));
        QPointF p2(m_scaleRadius * std::cos(angleRad), m_scaleRadius * std::sin(angleRad));
        painter->drawLine(p1, p2);

        // Draw numbers for: 5, 10, 20, 30, 40, 50, 60, 70 (NOT 0) - these are from val_for_logic
        if (val_for_logic == 5 || (val_for_logic >= 10 && val_for_logic <= displayedMax && val_for_logic % 10 == 0))
        {
            QString numStr = QString::number(val_for_logic); // Use val_for_logic for the text
            QRectF textRect = fm.boundingRect(numStr);
            float textX = m_numberRadius * std::cos(angleRad);
            float textY = m_numberRadius * std::sin(angleRad);
            textRect.moveCenter(QPointF(textX, textY)); 
            painter->drawText(textRect, Qt::AlignCenter, numStr);
        }
    }

    painter->restore();
}

void Mercedes190ETachometer::drawRedZone(QPainter *painter) {
    painter->save();

    float arcRadius = m_scaleRadius - (m_redZoneArcWidth / 2.0f);
    QPen redPen(QColor(220, 0, 0), m_redZoneArcWidth, Qt::SolidLine, Qt::FlatCap);
    painter->setPen(redPen);

    float startAngle = -1.0f * valueToAngle(_cfg.redline_rpm);
    float endAngle = -1.0f * valueToAngle(_cfg.max_rpm);
    float spanAngle = endAngle - startAngle;

    QRectF rect(-arcRadius, -arcRadius, 2 * arcRadius, 2 * arcRadius);
    painter->drawArc(rect, static_cast<int>(startAngle * 16), static_cast<int>(spanAngle * 16));

    painter->restore();
}

void Mercedes190ETachometer::drawStaticText(QPainter *painter) {
    painter->save();
    painter->setFont(m_labelFont);
    painter->setPen(Qt::white);
    QFontMetrics fm(m_labelFont);

    QString text1 = "x100";
    QString text2 = "1/min";

    float textAngleRad = degreesToRadians(-90.f);
    float radialDist = m_pivotRadius + 20.0f;

    QPointF basePos(radialDist * std::cos(textAngleRad), radialDist * std::sin(textAngleRad));
    
    QRect textRect1 = fm.boundingRect(text1);
    QPointF textPos1(basePos.x() - textRect1.width() / 2.0f, basePos.y() - textRect1.height() / 2.0f - fm.descent());
    painter->drawText(textPos1, text1);

    QRect textRect2 = fm.boundingRect(text2);
    QPointF textPos2(basePos.x() - textRect2.width() / 2.0f, basePos.y() + textRect2.height() / 2.0f - fm.descent());
    painter->drawText(textPos2, text2);

    painter->restore();
}

void Mercedes190ETachometer::drawNeedle(QPainter *painter) {
    painter->save();
    // Use tachometer's valueToAngle and m_currentRpmValue (which is now direct RPM)
    float angle = valueToAngle(m_currentRpmValue);
    painter->rotate(angle);

    // Needle properties (Orange, tapered - from Mercedes190ESpeedometer)
    QColor needleColor(255, 165, 0); // Orange
    float needleBaseWidth = 4.0f;    // Width at the pivot
    float needleTipWidth = 2.0f;     // Width at the tip

    QPolygonF needlePolygon;
    // Use m_needleLength from this class
    needlePolygon << QPointF(0.0f, -needleBaseWidth / 2.0f)        // Bottom-left at pivot
                  << QPointF(m_needleLength, -needleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(m_needleLength, needleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, needleBaseWidth / 2.0f);       // Top-left at pivot

    painter->setPen(Qt::NoPen); // No border for the needle itself
    painter->setBrush(needleColor);
    painter->drawPolygon(needlePolygon);
    
    // Central pivot (dark grey/black, flat circle)
    float pivotRadius = 8.0f; // Larger pivot as per image
    painter->setBrush(QColor(40, 40, 40)); // Dark grey
    // painter->setPen(QColor(20,20,20)); // Optional subtle border for pivot
    painter->drawEllipse(QPointF(0.0f,0.0f), pivotRadius, pivotRadius); 

    painter->restore();
}

void Mercedes190ETachometer::updateClockTime() {
    m_currentTime = QTime::currentTime();
    update(); // Trigger a repaint
}

void Mercedes190ETachometer::drawClock(QPainter *painter) {
    painter->save();

    // Clock properties
    const float clockCenterX = 0.0f;      // Centered with tachometer
    const float clockCenterY = 55.0f;     // Positioned below tachometer center
    const float clockRadius = 35.0f;      // Radius of the clock face (user updated)

    const float hourHandLength = 25.0f; // Adjusted for better proportion with new style
    const float minuteHandLength = 32.5f; // Adjusted for better proportion with new style

    const float tickLength = 3.0f;
    const float majorTickLength = 4.0f; // Longer ticks for 3,6,9,12
    const QColor clockTickColor = Qt::white;
    const QColor clockNumberColor = Qt::white;
    const float clockNumberRadius = clockRadius - tickLength - 7.0f; // Radius for placing numbers, adjusted

    // Needle style properties (copied from tachometer, scaled down)
    const QColor handColor(255, 165, 0); // Orange, same as tachometer
    const float hourHandBaseWidth = 2.5f;
    const float hourHandTipWidth = 1.0f;
    const float minuteHandBaseWidth = 2.0f;
    const float minuteHandTipWidth = 0.5f;
    const float clockPivotRadius = 5.0f;  // Smaller pivot for the clock
    const QColor pivotColor(40, 40, 40); // Dark grey, same as tachometer

    painter->translate(clockCenterX, clockCenterY);

    // Font for clock numbers
    QFont clockNumberFont(m_fontFamily, 6, QFont::Normal);
    QFontMetrics fm(clockNumberFont);
    painter->setFont(clockNumberFont);

    // Draw clock tick marks and numbers
    for (int i = 0; i < 12; ++i) { // 12 hours
        float angleDeg = static_cast<float>(i) * 30.0f - 90.0f; // -90 to make 0 (12 o'clock) point upwards
        float angleRad = degreesToRadians(angleDeg);

        bool isMajorHour = (i % 3 == 0); // True for 0 (12), 3, 6, 9
        float currentTickLength = isMajorHour ? majorTickLength : tickLength;

        painter->setPen(QPen(clockTickColor, isMajorHour ? 2.0f : 1.0f));
        QPointF p1((clockRadius - currentTickLength) * std::cos(angleRad), (clockRadius - currentTickLength) * std::sin(angleRad));
        QPointF p2(clockRadius * std::cos(angleRad), clockRadius * std::sin(angleRad));
        painter->drawLine(p1, p2);

        // Draw numbers for 12, 3, 6, 9
        if (isMajorHour) {
            int hourNumber = (i == 0) ? 12 : i; // Display '12' for i=0
            QString numStr = QString::number(hourNumber);
            QRectF textRect = fm.boundingRect(numStr);
            float textX = clockNumberRadius * std::cos(angleRad);
            float textY = clockNumberRadius * std::sin(angleRad);
            textRect.moveCenter(QPointF(textX, textY));
            painter->setPen(clockNumberColor);
            painter->drawText(textRect, Qt::AlignCenter, numStr);
        }
    }
    
    // Draw hour hand
    painter->save(); // Save for hour hand rotation
    painter->setPen(Qt::NoPen);
    painter->setBrush(handColor);
    float hourAngle = (m_currentTime.hour() % 12 + m_currentTime.minute() / 60.0f) * 30.0f - 90.0f; // 30 degrees per hour, -90 to start at 12
    painter->rotate(hourAngle);
    QPolygonF hourHandPolygon;
    hourHandPolygon << QPointF(0.0f, -hourHandBaseWidth / 2.0f)
                    << QPointF(hourHandLength, -hourHandTipWidth / 2.0f)
                    << QPointF(hourHandLength, hourHandTipWidth / 2.0f)
                    << QPointF(0.0f, hourHandBaseWidth / 2.0f);
    painter->drawPolygon(hourHandPolygon);
    painter->restore(); // Restore from hour hand rotation

    // Draw minute hand
    painter->save(); // Save for minute hand rotation
    painter->setPen(Qt::NoPen);
    painter->setBrush(handColor);
    float minuteAngle = (m_currentTime.minute()) * 6.0f - 90.0f; // 6 degrees per minute, -90 to start at 12
    painter->rotate(minuteAngle);
    QPolygonF minuteHandPolygon;
    minuteHandPolygon << QPointF(0.0f, -minuteHandBaseWidth / 2.0f)
                      << QPointF(minuteHandLength, -minuteHandTipWidth / 2.0f)
                      << QPointF(minuteHandLength, minuteHandTipWidth / 2.0f)
                      << QPointF(0.0f, minuteHandBaseWidth / 2.0f);
    painter->drawPolygon(minuteHandPolygon);
    painter->restore(); // Restore from minute hand rotation

    // Draw central pivot for the clock
    painter->setPen(Qt::NoPen);
    painter->setBrush(pivotColor);
    painter->drawEllipse(QPointF(0.0f, 0.0f), clockPivotRadius, clockPivotRadius);

    painter->restore(); // Restore from clock translation
}

#include "mercedes_190e_tachometer/moc_mercedes_190e_tachometer.cpp"
