#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "helpers.h"

#include <QPaintEvent>
#include <QMetaObject>
#include <QFontDatabase>

#include <spdlog/spdlog.h>

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>

// Expression parser
#include "expression_parser/expression_parser.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

Mercedes190ESpeedometer::Mercedes190ESpeedometer(const Mercedes190ESpeedometerConfig_t& cfg, QWidget *parent):
    QWidget(parent),
    current_speed_mph_(0.0f),
    cfg_{cfg},
    odometer_value_(cfg.odometer_value)
{
    // Initialize speed expression parser
    try
    {
        speed_expression_parser_ = std::make_unique<expression_parser::ExpressionParser>(
            cfg_.schema_type, 
            cfg_.speed_expression
        );
        
        if (!speed_expression_parser_->isValid())
        {
            SPDLOG_ERROR("Invalid speed expression '{}' for schema '{}' in speedometer", 
                        cfg_.speed_expression, cfg_.schema_type);
            speed_expression_parser_.reset(); // Disable expression parsing
        }
        else
        {
            SPDLOG_INFO("Speedometer initialized with speed expression: '{}' (schema: '{}')", 
                       cfg_.speed_expression, cfg_.schema_type);
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to initialize speed expression parser for speedometer: {}", e.what());
        speed_expression_parser_.reset();
    }
    
    // Initialize odometer expression parser (optional)
    if (!cfg_.odometer_expression.empty())
    {
        try
        {
            odometer_expression_parser_ = std::make_unique<expression_parser::ExpressionParser>(
                cfg_.schema_type, 
                cfg_.odometer_expression
            );
            
            if (!odometer_expression_parser_->isValid())
            {
                SPDLOG_ERROR("Invalid odometer expression '{}' for schema '{}' in speedometer", 
                            cfg_.odometer_expression, cfg_.schema_type);
                odometer_expression_parser_.reset();
            }
            else
            {
                SPDLOG_INFO("Speedometer initialized with odometer expression: '{}' (schema: '{}')", 
                           cfg_.odometer_expression, cfg_.schema_type);
            }
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to initialize odometer expression parser for speedometer: {}", e.what());
            odometer_expression_parser_.reset();
        }
    }
    
    // Load font from Qt resources
    QString font_family = "sans-serif";
    int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf"); // Use resource path
    if (fontId != -1)
    {
        font_family = QFontDatabase::applicationFontFamilies(fontId).at(0);
    }
    else
    {
        SPDLOG_WARN("Failed to load futura.ttf from resources. Using default sans-serif.");
    }

    // Set up the odometer font
    odo_font_ = QFont(font_family);
    odo_font_.setPointSizeF(11.0f); 
    odo_font_.setBold(true);

    // Set up the mph font
    mph_font_ = QFont(font_family);
    mph_font_.setPointSizeF(10.0f);

    // Set up the kmh font
    kmh_font_ = QFont(font_family);
    kmh_font_.setPointSizeF(6.0f);

    // Set up the miles font
    miles_font_ = QFont(font_family);
    miles_font_.setPointSizeF(7.0f);

    // Set up the kmh text font
    kmh_text_font_ = QFont(font_family);
    kmh_text_font_.setPointSizeF(7.0f);

    // Set up the unit font
    unit_font_ = QFont(font_family);
    unit_font_.setPointSizeF(9.0f);
    unit_font_.setBold(true);

    // Set up the vdo font
    vdo_font_ = QFont(font_family);
    vdo_font_.setPointSizeF(4.5f);
}


void Mercedes190ESpeedometer::setSpeed(float speed) // speed in MPH
{
    current_speed_mph_ = std::clamp(speed, 0.0f, static_cast<float>(cfg_.max_speed));
    update();
}

float Mercedes190ESpeedometer::valueToAngle(float value, float maxVal)
{
    float constrainedValue = std::clamp(value, 0.0f, maxVal);
    float factor = 0.0f;
    
    if (maxVal != 0.0f)
    {
        factor = constrainedValue / maxVal;
    }

    return kAngleMinDeg + factor * kAngleSweepDeg;
}

void Mercedes190ESpeedometer::setOdometerValue(int value)
{
    odometer_value_ = std::clamp(value, 0, 999999);
    update();
}

void Mercedes190ESpeedometer::paintEvent(QPaintEvent */*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int side = std::min(width(), height());
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

    constexpr float totalDigitsWidth = kNumDigits * kDigitWidth + (kNumDigits - 1) * kDigitSpacing;

    // Define the overall cutout area for the odometer
    constexpr float cutoutPadding = 2.0f; // Padding around the digits for the cutout
    constexpr float cutoutWidth = totalDigitsWidth + 2 * cutoutPadding;
    constexpr float cutoutHeight = kDigitHeight + 2 * cutoutPadding;
    constexpr float cutoutX = -cutoutWidth / 2.0f;
    constexpr float cutoutY = -30.0f - cutoutPadding; // Position based on original digit Y and padding

    constexpr QRectF cutoutRect(cutoutX, cutoutY, cutoutWidth, cutoutHeight);

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
    constexpr float digitStartX = cutoutX + cutoutPadding;
    constexpr float digitStartY = cutoutY + cutoutPadding;

    QString odoStr = QString::number(odometer_value_).rightJustified(kNumDigits, '0');

    painter->setFont(odo_font_);
    QFontMetricsF fm(odo_font_);

    for (uint8_t i = 0; i < kNumDigits; ++i)
    {
        float currentDigitX = digitStartX + i * (kDigitWidth + kDigitSpacing);
        QRectF digitWheelRect(currentDigitX, digitStartY, kDigitWidth, kDigitHeight);

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

void Mercedes190ESpeedometer::drawBoxesAtMPH(QPainter *painter, float mphValue, int numBoxes)
{
    float rawAngle = valueToAngle(mphValue, static_cast<float>(cfg_.max_speed));
    
    painter->save();
    painter->rotate(-rawAngle); // Rotate context so the direction of the value is along the +X axis
    painter->translate(kBoxMarkerRadius, 0); // Move out to the radius along this new +X axis

    float totalTangentialLength = numBoxes * kMarkerBoxSquareSize + (numBoxes - 1) * kBoxSpacing;
    float startY = -totalTangentialLength / 2.0f + kMarkerBoxSquareSize / 2.0f;

    // The rectangle is defined with its center at (0,0) in its own local space before translation
    // markerBoxSquareSize is radial (local X after rotation), markerBoxSquareSize is tangential (local Y after rotation)
    constexpr QRectF markerRect(
        -1.0f * kMarkerBoxSquareSize / 2.0f,
        -1.0f * kMarkerBoxSquareSize / 2.0f,
        kMarkerBoxSquareSize,
        kMarkerBoxSquareSize
    );

    for (int i = 0; i < numBoxes; ++i)
    {
        float currentBoxCenterY = startY + i * (kMarkerBoxSquareSize + kBoxSpacing);
        
        painter->save();
        painter->translate(0, currentBoxCenterY); // Translate tangentially for this specific box
        painter->drawRect(markerRect);
        painter->restore();
    }

    painter->restore(); // Restores translate and rotate for this MPH value
};

void Mercedes190ESpeedometer::drawMphTicksAndNumbers(QPainter *painter)
{
    painter->save();

    QPen arcPen(Qt::white);
    arcPen.setWidthF(kArcThickness);
    painter->setPen(arcPen);
    painter->setBrush(Qt::NoBrush);

    painter->drawArc(QRectF(-kArcRadius, -kArcRadius, kArcRadius * 2.0f, kArcRadius * 2.0f),
                     static_cast<int>(kAngleMinDeg * 16.0f),
                     static_cast<int>(kAngleSweepDeg * 16.0f));

    // Draw special rectangular markers
    painter->save();
    painter->setPen(Qt::NoPen); // No border for the boxes
    painter->setBrush(Qt::white); // White boxes

    for (uint8_t i = 0; i < cfg_.shift_box_markers.size(); ++i)
    {
        drawBoxesAtMPH(painter, static_cast<float>(cfg_.shift_box_markers[i]), i + 1);
    }

    painter->restore(); // Restores pen and brush settings set before drawing markers

    painter->setPen(Qt::white); 
    
    painter->setFont(mph_font_);
    QFontMetricsF fm(mph_font_);

    const float majorTickPenWidth = 2.0f;
    const float minorTickPenWidth = 1.0f;

    for (int mph = 0; mph <= cfg_.max_speed; mph += 5)
    {
        // Iterate every 5 MPH
        // Skip drawing ticks beyond cfg_.max_speed if they are not also major interval markers like 120 for loop end condition
        // This loop condition allows 120 to be processed. Ticks slightly over might occur if cfg_.max_speed wasn't a multiple of 5.
        float rawAngle = valueToAngle(static_cast<float>(mph), static_cast<float>(cfg_.max_speed));
        float angleRadForTicks = degrees_to_radians(-1.0f * rawAngle); // Negate angle for tick math
        
        bool isMajorTick = (mph % 10 == 0);
        bool isMinorTick = (mph % 5 == 0); // All ticks in this loop will be at least minor

        if (isMinorTick) // Draw all 5mph interval ticks
        {
            float tickLen = isMajorTick ? kMajorTickLen : kMinorTickLen;
            QPen tickPen = painter->pen(); // Get current pen (should be white)
            tickPen.setWidthF(isMajorTick ? majorTickPenWidth : minorTickPenWidth);
            painter->setPen(tickPen);

            QPointF p1_mph((kArcRadius + 1.0f) * std::cos(angleRadForTicks), (kArcRadius + 1.0f) * std::sin(angleRadForTicks));
            QPointF p2_mph((kArcRadius + tickLen) * std::cos(angleRadForTicks), (kArcRadius + tickLen) * std::sin(angleRadForTicks)); 
            painter->drawLine(p1_mph, p2_mph);
        }

        // Labels every 20 mph, starting from 20 up to maxSpeedMph
        if (mph % 20 == 0 && mph >= 0 && mph <= cfg_.max_speed)
        {
            QString strVal = QString::number(mph);
            float angleRadForNumbers_original = degrees_to_radians(rawAngle);
            float x_text_orig = kArcNumTextRadius * std::cos(angleRadForNumbers_original);
            float y_text_cartesian_orig = kArcNumTextRadius * std::sin(angleRadForNumbers_original);
            
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

    const float minSpeedKmh = 0.0f;
    const float maxSpeedKmh = mph_to_kph<float>(cfg_.max_speed); // Derived max KM/H

    QPen arcPen(Qt::white);
    arcPen.setWidthF(kKmhArcThickness);
    painter->setPen(arcPen);
    painter->setBrush(Qt::NoBrush);

    // Arc still uses the gauge's defined sweep
    painter->drawArc(QRectF(-1.0f * kKmhArcRadius, -1.0f * kKmhArcRadius, kKmhArcRadius * 2.0f, kKmhArcRadius * 2.0f),
                     static_cast<int>(kAngleMinDeg * 16.0f),
                     static_cast<int>(kAngleSweepDeg * 16.0f));

    painter->setPen(Qt::white);
 
    painter->setFont(kmh_font_);
    QFontMetricsF fm(kmh_font_);

    // KMH Ticks and Numbers
    // Iterate by 10 km/h for minor ticks, 20 km/h for major ticks/numbers
    for (float kmh = minSpeedKmh; kmh <= maxSpeedKmh + 1.0f /*allow last tick*/; kmh += 10.0f)
    {
        // Calculate angle for the current KM/H value based on the derived KM/H range
        float rawAngle = valueToAngle(kmh, maxSpeedKmh);
        float angleRadForTicks = degrees_to_radians(-rawAngle); // Negate for visual orientation
        
        bool isMajorTick = (static_cast<int>(kmh + 0.5f) % 20 == 0 && kmh >= minSpeedKmh); // Adding 0.5 for float comparison robustness
        bool isMinorTick = (static_cast<int>(kmh + 0.5f) % 10 == 0 && kmh >= minSpeedKmh);

        if (isMajorTick || isMinorTick)
        {
            float tickLen = isMajorTick ? kKmhMajorTickLen : kKmhMinorTickLen;
            QPointF p1_kmh(kKmhArcRadius * std::cos(angleRadForTicks), kKmhArcRadius * std::sin(angleRadForTicks));
            QPointF p2_kmh((kKmhArcRadius - tickLen) * std::cos(angleRadForTicks), (kKmhArcRadius - tickLen) * std::sin(angleRadForTicks)); 
            painter->drawLine(p1_kmh, p2_kmh);
        }

        if (isMajorTick && kmh > minSpeedKmh - 1.0f /*allow 0 to be skipped if desired by >0 logic*/)
        { 
            // Ensure we don't print numbers beyond max visible KM/H if they fall outside due to rounding
            if (kmh > maxSpeedKmh + 1.0f && static_cast<int>(kmh) %20 !=0)
            {
                continue;
            }

            if (kmh > maxSpeedKmh && static_cast<int>(kmh)%20==0 && kmh > maxSpeedKmh + 10.0f)
            {
                continue; // don't draw if too far over
            }

            QString strVal = QString::number(static_cast<int>(kmh + 0.5f));
            float angleRadForNumbers_original = degrees_to_radians(rawAngle);
            float x_text_orig = kKmhArcNumTextRadius * std::cos(angleRadForNumbers_original);
            float y_text_cartesian_orig = kKmhArcNumTextRadius * std::sin(angleRadForNumbers_original);

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

    // "miles" text - adjust Y if new pivot is larger
    
    painter->setFont(miles_font_);
    QFontMetricsF fmMiles(miles_font_);
    QString milesText = "miles";
    QRectF milesRect = fmMiles.boundingRect(milesText);
    // Y = -35 means 35 units UP from center in Y-down system
    milesRect.moveCenter(QPointF(0, -35)); 
    painter->drawText(milesRect, Qt::AlignCenter, milesText);
 

    painter->setFont(kmh_text_font_);
    QFontMetricsF fmKmh(kmh_text_font_);
 
    QString kmhText = "km/h";
    QRectF kmhRect = fmKmh.boundingRect(kmhText);
    kmhRect.moveCenter(QPointF(0, 30)); 
    painter->drawText(kmhRect, Qt::AlignCenter, kmhText);
  
    painter->setFont(unit_font_);
    QFontMetricsF fmUnits(unit_font_);

    QString mphText = "mph";
    QRectF mphRect = fmUnits.boundingRect(mphText);
    mphRect.moveCenter(QPointF(0, kmhRect.bottom() + fmUnits.height() * 0.8f)); 
    painter->drawText(mphRect, Qt::AlignCenter, mphText);
    
    painter->setFont(vdo_font_);
    QFontMetricsF fmVDO(vdo_font_);

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
    float rawNeedleAngle = valueToAngle(current_speed_mph_, static_cast<float>(cfg_.max_speed));
    painter->rotate(-rawNeedleAngle); 

    // Needle properties (Orange, tapered)
    QPolygonF needlePolygon;
    needlePolygon << QPointF(0.0f, -kNeedleBaseWidth / 2.0f)  // Bottom-left at pivot
                  << QPointF(kNeedleLength, -kNeedleTipWidth / 2.0f) // Bottom-right at tip
                  << QPointF(kNeedleLength, kNeedleTipWidth / 2.0f)  // Top-right at tip
                  << QPointF(0.0f, kNeedleBaseWidth / 2.0f);   // Top-left at pivot

    painter->setPen(Qt::NoPen); // No border for the needle itself
    painter->setBrush(kNeedleColor);
    painter->drawPolygon(needlePolygon);
    
    // Central pivot (dark grey/black, flat circle)
    painter->setBrush(kPivotColor); // Dark grey
    painter->drawEllipse(QPointF(0.0f,0.0f), kPivotRadius, kPivotRadius); 

    painter->restore();
}

void Mercedes190ESpeedometer::setZenohSession(std::shared_ptr<zenoh::Session> session)
{
    zenoh_session_ = session;
    
    // If we have a zenoh key configured, create the subscription
    if (!cfg_.zenoh_key.empty())
    {
        createZenohSubscription();
    }
}

void Mercedes190ESpeedometer::createZenohSubscription()
{
    if (!zenoh_session_)
    {
        SPDLOG_WARN("Cannot create subscription - no Zenoh session");
        return;
    }
    
    if (cfg_.zenoh_key.empty())
    {
        return; // No key configured
    }
    
    try
    {
        auto key_expr = zenoh::KeyExpr(cfg_.zenoh_key);
        
        zenoh_subscriber_ = std::make_unique<zenoh::Subscriber<void>>(
            zenoh_session_->declare_subscriber(
                key_expr,
                [this](const zenoh::Sample& sample)
                {
                    try
                    {
                        // Get the payload bytes
                        auto bytes = sample.get_payload().as_string();

                        // Use Qt's queued connection to ensure thread safety
                        QMetaObject::invokeMethod(this, "onDataReceived", Qt::QueuedConnection, bytes);
                        
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Error parsing speedometer data: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        SPDLOG_INFO("Created subscription for key '{}' with schema type '{}'", 
                    cfg_.zenoh_key, cfg_.schema_type);
        
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to create subscription for key '{}': {}", cfg_.zenoh_key, e.what());
    }
}

void Mercedes190ESpeedometer::onDataReceived(const std::string& bytes)
{
    try
    {
        // Convert string bytes to vector<uint8_t> for expression parser
        std::vector<uint8_t> payload(bytes.begin(), bytes.end());
        
        // Evaluate speed expression
        if (speed_expression_parser_)
        {
            float speedMph = speed_expression_parser_->evaluate<float>(payload);
            setSpeed(speedMph);
        }
        
        // Evaluate odometer expression (if configured)
        if (odometer_expression_parser_)
        {
            int odometerValue = odometer_expression_parser_->evaluate<int>(payload);
            setOdometerValue(odometerValue);
        }
        
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Speedometer: Failed to evaluate expressions: {}", e.what());
    }
}

#include "mercedes_190e_speedometer/moc_mercedes_190e_speedometer.cpp"
