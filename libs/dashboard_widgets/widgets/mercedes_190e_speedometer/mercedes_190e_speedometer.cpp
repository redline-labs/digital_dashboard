#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "helpers/unit_conversion.h"

#include <QPaintEvent>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "dashboard/gauge_painting.h"
#include "qt_helpers/widget_fonts.h"

#include <algorithm>
#include <chrono>
#include <cmath>

Mercedes190ESpeedometer::Mercedes190ESpeedometer(const Mercedes190ESpeedometerConfig_t& cfg, QWidget *parent):
    qt_helpers::CachedPaintWidget(parent),
    current_speed_mph_(0.0f),
    cfg_{cfg},
    odometer_value_(cfg.odometer_value)
{
    speed_expression_parser_ = dashboard::makeExpressionSubscription<float>(
        cfg_.schema_type, cfg_.speed_expression, cfg_.zenoh_key,
        this, &Mercedes190ESpeedometer::setSpeed,
        std::chrono::milliseconds(cfg_.speed_stale_after_ms));

    odometer_expression_parser_ = dashboard::makeExpressionSubscription<int>(
        cfg_.odometer_schema_type, cfg_.odometer_expression, cfg_.odometer_zenoh_key,
        this, &Mercedes190ESpeedometer::setOdometerValue,
        std::chrono::milliseconds(cfg_.odometer_stale_after_ms));

    QString font_family = qt_helpers::loadResourceFont(":/fonts/futura.ttf", "sans-serif");

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
    // clampToRange, not std::clamp: it refuses to pass a non-finite value into
    // the needle rotation, which std::clamp does. And only repaint when the
    // needle actually moves -- this ran on every sample regardless.
    const float clamped = gauge_paint::clampToRange(speed, 0.0f, static_cast<float>(cfg_.max_speed));
    if (clamped == current_speed_mph_)
    {
        return;
    }
    current_speed_mph_ = clamped;
    update();
}

float Mercedes190ESpeedometer::valueToAngle(float value, float maxVal)
{
    return gauge_paint::valueToAngleDeg(value, 0.0f, maxVal, kAngleMinDeg, kAngleSweepDeg);
}

void Mercedes190ESpeedometer::setOdometerValue(int value)
{
    // Six digits are rendered, so anything past that is not displayable. The
    // clamp also makes the value non-negative, so it fits the unsigned member.
    const auto clamped = static_cast<uint32_t>(std::clamp(value, 0, 999999));
    if (clamped == odometer_value_)
    {
        return;
    }
    odometer_value_ = clamped;
    update();
}

void Mercedes190ESpeedometer::applyPaintTransform(QPainter& painter) const
{
    gauge_paint::applyCenteredScale(painter, *this);
}

void Mercedes190ESpeedometer::paintStaticUnderlay(QPainter& painter)
{
    gauge_paint::drawCircularBackground(painter);
    drawMphTicksAndNumbers(&painter);
    drawKmhTicksAndNumbers(&painter);
    drawOverlayText(&painter);
}

void Mercedes190ESpeedometer::paintDynamic(QPainter& painter)
{
    drawOdometer(&painter); // Dynamic digits

    // No needle at all while the speed stream is quiet: a needle parked at zero
    // is a reading, and this dial has no other way to say it has none.
    if (speedStale())
    {
        gauge_paint::drawNoDataLegend(painter, QRectF(-70.0, 30.0, 140.0, 24.0), painter.font());
        return;
    }

    drawNeedle(&painter); // Draw needle last so it's on top
}

namespace
{
constexpr float kCutoutPadding = 2.0f; // Padding around the digits for the cutout
}  // namespace

QRectF Mercedes190ESpeedometer::odometerCutoutRect()
{
    constexpr float totalDigitsWidth = kNumDigits * kDigitWidth + (kNumDigits - 1) * kDigitSpacing;
    constexpr float cutoutWidth = totalDigitsWidth + 2 * kCutoutPadding;
    constexpr float cutoutHeight = kDigitHeight + 2 * kCutoutPadding;
    constexpr float cutoutX = -cutoutWidth / 2.0f;
    constexpr float cutoutY = -30.0f - kCutoutPadding; // Position based on original digit Y and padding
    return QRectF(cutoutX, cutoutY, cutoutWidth, cutoutHeight);
}

void Mercedes190ESpeedometer::drawOdometer(QPainter *painter)
{
    // The drums change about once a second and the needle every frame, so
    // they are drawn once into a pixmap and blitted. Redrawing the cutout and
    // six wheels of text each frame was a tenth of this process's paint time.
    // Keyed on what they show and on where they land, in device pixels.
    const QString text = odometerStale()
                             ? QString(kNumDigits, QLatin1Char('-'))
                             : QString::number(odometer_value_).rightJustified(kNumDigits, '0');
    const QTransform transform = painter->worldTransform();
    const qreal dpr = devicePixelRatioF();

    if (odometer_cache_.isNull() || text != odometer_cache_text_ || transform != odometer_cache_transform_ ||
        !qFuzzyCompare(dpr, odometer_cache_dpr_))
    {
        // Margin for the 1-unit border strokes, which straddle the cutout's
        // edge, and a pixel for their antialiasing.
        odometer_cache_rect_ =
            transform.mapRect(odometerCutoutRect().adjusted(-1.0, -1.0, 1.0, 1.0)).toAlignedRect().adjusted(-1, -1, 1, 1);

        // Greyscale-antialiased text, like every numeral in the cached
        // underlay: Qt keeps subpixel text for painting straight onto a widget.
        odometer_cache_ = QPixmap(odometer_cache_rect_.size() * dpr);
        odometer_cache_.setDevicePixelRatio(dpr);
        odometer_cache_.fill(Qt::transparent);

        QPainter cache(&odometer_cache_);
        cache.setRenderHints(painter->renderHints());
        cache.translate(-odometer_cache_rect_.topLeft());
        cache.setWorldTransform(transform, true);
        paintOdometer(&cache, text);

        odometer_cache_text_ = text;
        odometer_cache_transform_ = transform;
        odometer_cache_dpr_ = dpr;
    }

    painter->save();
    painter->resetTransform();
    painter->drawPixmap(odometer_cache_rect_.topLeft(), odometer_cache_);
    painter->restore();
}

void Mercedes190ESpeedometer::paintOdometer(QPainter *painter, const QString& odoStr)
{
    painter->save();

    const QRectF cutoutRect = odometerCutoutRect();

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
    const qreal digitStartX = cutoutRect.x() + kCutoutPadding;
    const qreal digitStartY = cutoutRect.y() + kCutoutPadding;

    painter->setFont(odo_font_);
    QFontMetricsF fm(odo_font_);

    for (uint8_t i = 0; i < kNumDigits; ++i)
    {
        const qreal currentDigitX = digitStartX + i * (kDigitWidth + kDigitSpacing);
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

void Mercedes190ESpeedometer::drawBoxesAtMPH(QPainter *painter, float mphValue, std::size_t numBoxes)
{
    float rawAngle = valueToAngle(mphValue, static_cast<float>(cfg_.max_speed));
    
    painter->save();
    painter->rotate(-rawAngle); // Rotate context so the direction of the value is along the +X axis
    painter->translate(kBoxMarkerRadius, 0); // Move out to the radius along this new +X axis

    const float boxCount = static_cast<float>(numBoxes);
    float totalTangentialLength = boxCount * kMarkerBoxSquareSize + (boxCount - 1.0f) * kBoxSpacing;
    float startY = -totalTangentialLength / 2.0f + kMarkerBoxSquareSize / 2.0f;

    // The rectangle is defined with its center at (0,0) in its own local space before translation
    // markerBoxSquareSize is radial (local X after rotation), markerBoxSquareSize is tangential (local Y after rotation)
    constexpr QRectF markerRect(
        -1.0f * kMarkerBoxSquareSize / 2.0f,
        -1.0f * kMarkerBoxSquareSize / 2.0f,
        kMarkerBoxSquareSize,
        kMarkerBoxSquareSize
    );

    for (std::size_t i = 0u; i < numBoxes; ++i)
    {
        float currentBoxCenterY = startY + static_cast<float>(i) * (kMarkerBoxSquareSize + kBoxSpacing);
        
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

    // std::size_t, not uint8_t: the counter used to be the same width as the
    // element type, so a config with 256 or more markers wrapped it back to 0
    // and this loop never ended.
    for (std::size_t i = 0; i < cfg_.shift_box_markers.size(); ++i)
    {
        drawBoxesAtMPH(painter, static_cast<float>(cfg_.shift_box_markers[i]), i + 1u);
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
        
        bool isMajorTick = (mph % 10 == 0);
        bool isMinorTick = (mph % 5 == 0); // All ticks in this loop will be at least minor

        if (isMinorTick) // Draw all 5mph interval ticks
        {
            float tickLen = isMajorTick ? kMajorTickLen : kMinorTickLen;
            QPen tickPen = painter->pen(); // Get current pen (should be white)
            tickPen.setWidthF(isMajorTick ? majorTickPenWidth : minorTickPenWidth);
            painter->setPen(tickPen);

            gauge_paint::drawRadialTick(*painter, rawAngle, kArcRadius + 1.0f, kArcRadius + tickLen);
        }

        // Labels every 20 mph, starting from 20 up to maxSpeedMph
        if (mph % 20 == 0 && mph >= 0 && mph <= cfg_.max_speed)
        {
            painter->setPen(Qt::white); // tick drawing above may have changed it
            gauge_paint::drawTextAtAngle(*painter, fm, rawAngle, kArcNumTextRadius, QString::number(mph));
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
        
        bool isMajorTick = (static_cast<int>(kmh + 0.5f) % 20 == 0 && kmh >= minSpeedKmh); // Adding 0.5 for float comparison robustness
        bool isMinorTick = (static_cast<int>(kmh + 0.5f) % 10 == 0 && kmh >= minSpeedKmh);

        if (isMajorTick || isMinorTick)
        {
            float tickLen = isMajorTick ? kKmhMajorTickLen : kKmhMinorTickLen;
            gauge_paint::drawRadialTick(*painter, rawAngle, kKmhArcRadius, kKmhArcRadius - tickLen);
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
    painter->rotate(-valueToAngle(current_speed_mph_, static_cast<float>(cfg_.max_speed)));
    gauge_paint::drawTaperedNeedle(*painter, kNeedleLength, kNeedleBaseWidth, kNeedleTipWidth, kNeedleColor);
    gauge_paint::drawPivot(*painter, kPivotRadius, kPivotColor);
    painter->restore();
}

#include "mercedes_190e_speedometer/moc_mercedes_190e_speedometer.cpp"
