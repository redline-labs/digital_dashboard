#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include <QPaintEvent>
#include <QFontMetrics>
#include <QDebug> // For font loading messages
#include <spdlog/spdlog.h>

#include <cmath>
#include <numbers>
#include <vector> // For std::vector

// Helper for degree to radian conversion
constexpr float degreesToRadians(float degrees)
{
    return degrees * (std::numbers::pi_v<float> / 180.0f);
}

Mercedes190ESpeedometer::Mercedes190ESpeedometer(const Mercedes190ESpeedometerConfig_t& cfg, QWidget *parent)
    : QWidget(parent), m_currentSpeedMph(0.0f), _cfg{cfg}, m_odometerValue(cfg.odometer_value) // Initial odometer value
{
    // Load font from Qt resources
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf"); // Use resource path
    if (fontId != -1) {
        m_fontFamily = QFontDatabase::applicationFontFamilies(fontId).at(0);
    } else {
        qWarning("Failed to load futura.ttf from resources. Using default sans-serif.");
        m_fontFamily = "sans-serif"; // Fallback font
    }
}

void Mercedes190ESpeedometer::setSpeed(float speed) // speed in MPH
{
    m_currentSpeedMph = qBound(0.0f, speed, static_cast<float>(_cfg.max_speed));
    update();
}

float Mercedes190ESpeedometer::getSpeed() const
{
    return m_currentSpeedMph;
}

float Mercedes190ESpeedometer::valueToAngle(float value, float maxVal)
{
    float constrainedValue = qBound(0.0f, value, maxVal);
    float factor = 0.0f;
    if (maxVal != 0.0f)
    {
        factor = constrainedValue / maxVal;
    }
    return m_angleValueMin + factor * m_angleSweep;
}

void Mercedes190ESpeedometer::setOdometerValue(int value)
{
    if (value >= 0 && value <= 999999) { // Assume 6 digits max
        m_odometerValue = value;
        update();
    }
}

void Mercedes190ESpeedometer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int side = qMin(width(), height());
    painter.translate(width() / 2.0, height() / 2.0); // Origin to center
    painter.scale(side / 200.0, side / 200.0); // Logical 200x200 unit square

    drawBackground(&painter);
    drawOdometer(&painter); // Draw odometer before some overlay elements but after background
    drawMphTicksAndNumbers(&painter);
    drawKmhTicksAndNumbers(&painter);
    drawOverlayText(&painter);
    drawNeedle(&painter); // Draw needle last so it's on top
}

void Mercedes190ESpeedometer::drawBackground(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawEllipse(QPointF(0.0f, 0.0f), 100.0f, 100.0f); // Background circle
    painter->restore();
}

void Mercedes190ESpeedometer::drawOdometer(QPainter *painter)
{
    painter->save();

    const int numDigits = 6;
    const float digitWidth = 12.0f;
    const float digitHeight = 18.0f; 
    const float digitSpacing = 0.0f; 
    const float totalDigitsWidth = numDigits * digitWidth + (numDigits - 1) * digitSpacing;

    // Define the overall cutout area for the odometer
    const float cutoutPadding = 2.0f; // Padding around the digits for the cutout
    const float cutoutWidth = totalDigitsWidth + 2 * cutoutPadding;
    const float cutoutHeight = digitHeight + 2 * cutoutPadding;
    const float cutoutX = -cutoutWidth / 2.0f;
    const float cutoutY = -30.0f - cutoutPadding; // Position based on original digit Y and padding

    QRectF cutoutRect(cutoutX, cutoutY, cutoutWidth, cutoutHeight);

    // 1. Draw the main inset effect for the cutout area
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(10, 10, 10)); // Dark base for the cutout area
    painter->drawRect(cutoutRect);

    // Inset border effect for the cutout
    // Top & Left shadow (gauge face casting shadow into recess)
    QPen shadowPen(QColor(0, 0, 0), 1.0f); // Black, solid shadow
    painter->setPen(shadowPen);
    painter->drawLine(cutoutRect.topLeft() + QPointF(0.0f,0.0f), cutoutRect.topRight() + QPointF(0.0f,0.0f));
    painter->drawLine(cutoutRect.topLeft() + QPointF(0.0f,0.0f), cutoutRect.bottomLeft() + QPointF(0.0f,0.0f));

    // Bottom & Right highlight (light catching inner edge of recess)
    QPen highlightPen(QColor(60, 60, 60), 1.0f); // Dark grey highlight
    painter->setPen(highlightPen);
    painter->drawLine(cutoutRect.topRight() + QPointF(-1.0f,1.0f), cutoutRect.bottomRight() + QPointF(-1.0f,0.0f)); // Offset for inner edge
    painter->drawLine(cutoutRect.bottomLeft() + QPointF(1.0f,-1.0f), cutoutRect.bottomRight() + QPointF(0.0f,-1.0f)); // Offset for inner edge
    

    // 2. Draw the individual digit wheels within this cutout
    const float digitStartX = cutoutX + cutoutPadding;
    const float digitStartY = cutoutY + cutoutPadding;

    QString odoStr = QString::number(m_odometerValue).rightJustified(numDigits, '0');

    QFont odoFont(m_fontFamily);
    odoFont.setPointSizeF(11.0f); 
    odoFont.setBold(true);
    painter->setFont(odoFont);
    QFontMetricsF fm(odoFont);

    for (int i = 0; i < numDigits; ++i)
    {
        float currentDigitX = digitStartX + i * (digitWidth + digitSpacing);
        QRectF digitWheelRect(currentDigitX, digitStartY, digitWidth, digitHeight);

        // Background for individual wheel (can be slightly different or same as cutout base)
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(25, 25, 25)); 
        painter->drawRect(digitWheelRect);

        // Optional: very subtle edge for individual wheels if needed, or remove if cutout is enough
        QPen wheelEdgePen(QColor(50, 50, 50), 0.5f);
        painter->setPen(wheelEdgePen);
        painter->drawRect(digitWheelRect.adjusted(0,0,-1,-1)); // Draw inside to not overdraw main inset

        painter->setPen(Qt::white);
        QString digitChar = odoStr.at(i);
        QRectF textBoundingRect = fm.tightBoundingRect(digitChar);
        QPointF textPos(digitWheelRect.center().x() - textBoundingRect.width() / 2.0 - textBoundingRect.left(),
                        digitWheelRect.center().y() - textBoundingRect.height() / 2.0 - textBoundingRect.top());
        painter->drawText(textPos, digitChar);
    }

    painter->restore();
}

void Mercedes190ESpeedometer::drawMphTicksAndNumbers(QPainter *painter)
{
    painter->save();
    
    float mphArcRadius = 70.0f;
    float mphArcThickness = 1.25f;
    float mphNumTextRadius = 85.0f; 
    float majorTickLen = 6.0f; 
    float minorTickLen = 4.0f; 

    QPen arcPen(Qt::white);
    arcPen.setWidthF(mphArcThickness);
    painter->setPen(arcPen);
    painter->setBrush(Qt::NoBrush);

    painter->drawArc(QRectF(-mphArcRadius, -mphArcRadius, mphArcRadius * 2.0f, mphArcRadius * 2.0f),
                     static_cast<int>(m_angleValueMin * 16.0f), 
                     static_cast<int>(m_angleSweep * 16.0f));

    // Draw special rectangular markers
    painter->save();
    painter->setPen(Qt::NoPen); // No border for the boxes
    painter->setBrush(Qt::white); // White boxes

    const float boxMarkerRadius = 67.5f; // Radius for the center of the boxes
    const float markerBoxSquareSize = 2.0f;  // Length of each side of the square.
    const float boxSpacing = 1.0f;      // Spacing between multiple boxes

    auto drawBoxesAtMPH = [&](float mphValue, int numBoxes) {
        float rawAngle = valueToAngle(mphValue, static_cast<float>(_cfg.max_speed));
        
        painter->save();
        painter->rotate(-rawAngle); // Rotate context so the direction of the value is along the +X axis
        painter->translate(boxMarkerRadius, 0); // Move out to the radius along this new +X axis

        float totalTangentialLength = numBoxes * markerBoxSquareSize + (numBoxes - 1) * boxSpacing;
        float startY = -totalTangentialLength / 2.0f + markerBoxSquareSize / 2.0f;

        for (int i = 0; i < numBoxes; ++i) {
            float currentBoxCenterY = startY + i * (markerBoxSquareSize + boxSpacing);
            // The rectangle is defined with its center at (0,0) in its own local space before translation
            // markerBoxSquareSize is radial (local X after rotation), markerBoxSquareSize is tangential (local Y after rotation)
            QRectF markerRect(-markerBoxSquareSize / 2.0f, -markerBoxSquareSize / 2.0f, markerBoxSquareSize, markerBoxSquareSize);
            
            painter->save();
            painter->translate(0, currentBoxCenterY); // Translate tangentially for this specific box
            painter->drawRect(markerRect);
            painter->restore();
        }
        painter->restore(); // Restores translate and rotate for this MPH value
    };

    drawBoxesAtMPH(28.0f, 1);
    drawBoxesAtMPH(54.0f, 2);
    drawBoxesAtMPH(87.0f, 3);

    painter->restore(); // Restores pen and brush settings set before drawing markers

    painter->setPen(Qt::white); 
    QFont mphFont(m_fontFamily); // Use loaded font family
    mphFont.setPointSizeF(10.0f);
    painter->setFont(mphFont);
    QFontMetricsF fm(mphFont);

    const float majorTickPenWidth = 2.0f;
    const float minorTickPenWidth = 1.0f;

    for (int mph = 0; mph <= _cfg.max_speed; mph += 5) { // Iterate every 5 MPH
        // Skip drawing ticks beyond _cfg.max_speed if they are not also major interval markers like 120 for loop end condition
        // This loop condition allows 120 to be processed. Ticks slightly over might occur if _cfg.max_speed wasn't a multiple of 5.

        float rawAngle = valueToAngle(static_cast<float>(mph), static_cast<float>(_cfg.max_speed));
        float angleRadForTicks = degreesToRadians(-rawAngle); // Negate angle for tick math
        
        bool isMajorTick = (mph % 10 == 0);
        bool isMinorTick = (mph % 5 == 0); // All ticks in this loop will be at least minor

        if (isMinorTick) { // Draw all 5mph interval ticks
            float tickLen = isMajorTick ? majorTickLen : minorTickLen;
            QPen tickPen = painter->pen(); // Get current pen (should be white)
            tickPen.setWidthF(isMajorTick ? majorTickPenWidth : minorTickPenWidth);
            painter->setPen(tickPen);

            QPointF p1_mph((mphArcRadius + 1.0f) * std::cos(angleRadForTicks), (mphArcRadius + 1.0f) * std::sin(angleRadForTicks));
            QPointF p2_mph((mphArcRadius + tickLen) * std::cos(angleRadForTicks), (mphArcRadius + tickLen) * std::sin(angleRadForTicks)); 
            painter->drawLine(p1_mph, p2_mph);
        }

        // Labels every 20 mph, starting from 20 up to maxSpeedMph
        if (mph % 20 == 0 && mph >= 0 && mph <= _cfg.max_speed) {
            // Special case for 0: only draw if it's exactly 0, often not labelled unless it's the start of a range shown
            // The prompt usually implies 20, 40, etc. for labels, so 0 might not be desired.
            // Let's draw 0 only if explicitly 0 and no other labels are drawn for it yet. Or, skip 0 entirely based on common gauge design.
            // The image does not show '0' for MPH.
            //if (mph == 0) continue; // Skip '0' MPH label based on image

            QString strVal = QString::number(mph);
            float angleRadForNumbers_original = degreesToRadians(rawAngle);
            float x_text_orig = mphNumTextRadius * std::cos(angleRadForNumbers_original);
            float y_text_cartesian_orig = mphNumTextRadius * std::sin(angleRadForNumbers_original);
            
            QRectF textRect = fm.boundingRect(strVal);
            textRect.moveCenter(QPointF(x_text_orig, -y_text_cartesian_orig)); 
            painter->setPen(Qt::white); // Ensure pen is white for text (might have been changed by tick drawing)
            painter->drawText(textRect, Qt::AlignCenter, strVal);
        }
    }
    painter->restore();
}

void Mercedes190ESpeedometer::drawKmhTicksAndNumbers(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::white);

    constexpr float mphToKmh = 1.60934f;
    const float minSpeedKmh = 0.0f;
    const float maxSpeedKmh = static_cast<float>(_cfg.max_speed) * mphToKmh; // Derived max KM/H

    float kmhArcRadius = 65.0f;
    float kmhArcThickness = 1.25f;
    float kmhNumTextRadius = 50.0f; 
    float kmhMajorTickLen = 6.0f;
    float kmhMinorTickLen = 3.0f;

    QPen arcPen(Qt::white);
    arcPen.setWidthF(kmhArcThickness);
    painter->setPen(arcPen);
    painter->setBrush(Qt::NoBrush);

    // Arc still uses the gauge's defined sweep
    painter->drawArc(QRectF(-kmhArcRadius, -kmhArcRadius, kmhArcRadius * 2.0f, kmhArcRadius * 2.0f),
                     static_cast<int>(m_angleValueMin * 16.0f),
                     static_cast<int>(m_angleSweep * 16.0f));

    painter->setPen(Qt::white);
    QFont kmhFontUser(m_fontFamily); // User specified font variable name
    kmhFontUser.setPointSizeF(6.0f); 
    painter->setFont(kmhFontUser);
    QFontMetricsF fm(kmhFontUser);

    // KMH Ticks and Numbers
    // Iterate by 10 km/h for minor ticks, 20 km/h for major ticks/numbers
    for (float kmh = minSpeedKmh; kmh <= maxSpeedKmh + 1.0f /*allow last tick*/; kmh += 10.0f)
    {
        // Ensure we don't draw too far past maxSpeedKmh if it's not a major interval
        if (kmh > maxSpeedKmh && static_cast<int>(kmh) % 20 != 0)
        {
             if (kmh - 10.0f < maxSpeedKmh) { // if the previous tick was before max, consider drawing the one just after max if it's major
                // allow one last major tick if it's just over the maxSpeedKmh
             } else {
                continue;
             }
        }
        if (kmh > maxSpeedKmh + 10.0f) continue; // Stop definitely after 10 over

        // Calculate angle for the current KM/H value based on the derived KM/H range
        float rawAngle = valueToAngle(kmh, maxSpeedKmh);
        float angleRadForTicks = degreesToRadians(-rawAngle); // Negate for visual orientation
        
        bool isMajorTick = (static_cast<int>(kmh + 0.5f) % 20 == 0 && kmh >= minSpeedKmh); // Adding 0.5 for float comparison robustness
        bool isMinorTick = (static_cast<int>(kmh + 0.5f) % 10 == 0 && kmh >= minSpeedKmh);

        if (isMajorTick || isMinorTick) {
            float tickLen = isMajorTick ? kmhMajorTickLen : kmhMinorTickLen;
            QPointF p1_kmh(kmhArcRadius * std::cos(angleRadForTicks), kmhArcRadius * std::sin(angleRadForTicks));
            QPointF p2_kmh((kmhArcRadius - tickLen) * std::cos(angleRadForTicks), (kmhArcRadius - tickLen) * std::sin(angleRadForTicks)); 
            painter->drawLine(p1_kmh, p2_kmh);
        }

        if (isMajorTick && kmh > minSpeedKmh - 1.0f /*allow 0 to be skipped if desired by >0 logic*/) { 
            // Ensure we don't print numbers beyond max visible KM/H if they fall outside due to rounding
            if (kmh > maxSpeedKmh + 1.0f && static_cast<int>(kmh) %20 !=0) continue;
            if (kmh > maxSpeedKmh && static_cast<int>(kmh)%20==0 && kmh > maxSpeedKmh + 10.0f) continue; // don't draw if too far over
            
            QString strVal = QString::number(static_cast<int>(kmh + 0.5f));
            float angleRadForNumbers_original = degreesToRadians(rawAngle);
            float x_text_orig = kmhNumTextRadius * std::cos(angleRadForNumbers_original);
            float y_text_cartesian_orig = kmhNumTextRadius * std::sin(angleRadForNumbers_original);

            QRectF textRect = fm.boundingRect(strVal);
            textRect.moveCenter(QPointF(x_text_orig, -y_text_cartesian_orig));
            painter->drawText(textRect, Qt::AlignCenter, strVal);
        }
    }
    painter->restore();
}

void Mercedes190ESpeedometer::drawOverlayText(QPainter *painter)
{
    painter->save();
    painter->setPen(Qt::white);

    QFont defaultFont(m_fontFamily); 

    // "miles" text - adjust Y if new pivot is larger
    QFont milesFont = defaultFont;
    milesFont.setPointSizeF(7.0f);
    painter->setFont(milesFont);
    QFontMetricsF fmMiles(milesFont);
    QString milesText = "miles";
    QRectF milesRect = fmMiles.boundingRect(milesText);
    // Y = -35 means 35 units UP from center in Y-down system
    milesRect.moveCenter(QPointF(0, -35)); 
    painter->drawText(milesRect, Qt::AlignCenter, milesText);
 
    QFont kmhTextFont = defaultFont; 
    kmhTextFont.setPointSizeF(7.0f);
    painter->setFont(kmhTextFont);
    QFontMetricsF fmKmh(kmhTextFont);
 
    QString kmhText = "km/h";
    QRectF kmhRect = fmKmh.boundingRect(kmhText);
    kmhRect.moveCenter(QPointF(0, 30)); 
    painter->drawText(kmhRect, Qt::AlignCenter, kmhText);
  
    QFont unitFont = defaultFont; 
    unitFont.setPointSizeF(9.0f);
    unitFont.setBold(true);
    painter->setFont(unitFont);
    QFontMetricsF fmUnits(unitFont);

    QString mphText = "mph";
    QRectF mphRect = fmUnits.boundingRect(mphText);
    mphRect.moveCenter(QPointF(0, kmhRect.bottom() + fmUnits.height() * 0.8f)); 
    painter->drawText(mphRect, Qt::AlignCenter, mphText);
    
    QFont vdoFont = defaultFont; 
    vdoFont.setPointSizeF(4.5f);
    painter->setFont(vdoFont);
    QFontMetricsF fmVDO(vdoFont);
    QString vdoLine1 = "\u24B8 201 542 4606"; 
    QString vdoLine2 = "VDO";
    QRectF vdo1Rect = fmVDO.boundingRect(vdoLine1);
    vdo1Rect.moveCenter(QPointF(0, mphRect.bottom() + fmVDO.height() * 1.0f));
    painter->drawText(vdo1Rect, Qt::AlignCenter, vdoLine1);
    QRectF vdo2Rect = fmVDO.boundingRect(vdoLine2);
    vdo2Rect.moveCenter(QPointF(0, vdo1Rect.bottom() + fmVDO.height() * 0.6f));
    painter->drawText(vdo2Rect, Qt::AlignCenter, vdoLine2);

    painter->restore(); 
}

void Mercedes190ESpeedometer::drawNeedle(QPainter *painter)
{
    painter->save();
    float rawNeedleAngle = valueToAngle(m_currentSpeedMph, static_cast<float>(_cfg.max_speed));
    painter->rotate(-rawNeedleAngle); 

    // Needle properties (Orange, tapered)
    QColor needleColor(255, 165, 0); // Orange
    float needleLength = 85.0f;      // Length from pivot to tip
    float needleBaseWidth = 4.0f;    // Width at the pivot
    float needleTipWidth = 2.0f;     // Width at the tip (can be 0 for a sharp point)

    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -needleBaseWidth / 2.0f)  // Bottom-left at pivot
                  << QPointF(needleLength, -needleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(needleLength, needleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, needleBaseWidth / 2.0f);   // Top-left at pivot

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

void Mercedes190ESpeedometer::setZenohSession(std::shared_ptr<zenoh::Session> session)
{
    _zenoh_session = session;
    
    // If we have a zenoh key configured, create the subscription
    if (!_cfg.zenoh_key.empty()) {
        createZenohSubscription();
    }
}

void Mercedes190ESpeedometer::createZenohSubscription()
{
    if (!_zenoh_session) {
        spdlog::warn("Mercedes190ESpeedometer: Cannot create subscription - no Zenoh session");
        return;
    }
    
    if (_cfg.zenoh_key.empty()) {
        return; // No key configured
    }
    
    try {
        auto key_expr = zenoh::KeyExpr(_cfg.zenoh_key);
        
        _zenoh_subscriber = std::make_unique<zenoh::Subscriber<void>>(
            _zenoh_session->declare_subscriber(
                key_expr,
                [this](const zenoh::Sample& sample) {
                    try {
                        // Convert payload to string
                        const auto& payload = sample.get_payload();
                        std::string data_str = payload.as_string();
                        
                        // Convert data to speed in m/s
                        double speed_mps = std::stod(data_str);
                        
                        // Use Qt's queued connection to ensure thread safety
                        QMetaObject::invokeMethod(this, "onSpeedDataReceived", 
                                                Qt::QueuedConnection, 
                                                Q_ARG(double, speed_mps));
                        
                    } catch (const std::exception& e) {
                        spdlog::error("Mercedes190ESpeedometer: Error parsing speed data: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        spdlog::info("Mercedes190ESpeedometer: Created subscription for key '{}'", _cfg.zenoh_key);
        
    } catch (const std::exception& e) {
        spdlog::error("Mercedes190ESpeedometer: Failed to create subscription for key '{}': {}", 
                     _cfg.zenoh_key, e.what());
    }
}

void Mercedes190ESpeedometer::onSpeedDataReceived(double speedMps)
{
    // Convert m/s to mph (1 m/s = 2.23694 mph)
    double speedMph = speedMps * 2.23694;
    setSpeed(static_cast<float>(speedMph));
}

#include "mercedes_190e_speedometer/moc_mercedes_190e_speedometer.cpp"
