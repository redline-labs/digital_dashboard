#ifndef SPEEDOMETERWIDGETMPH_H
#define SPEEDOMETERWIDGETMPH_H

#include "mercedes_190e_speedometer/config.h"
#include "zenoh.hxx"

#include <QWidget>
#include <QPainter>
#include <QPointF>
#include <QtMath>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QFontMetrics>

#include <string_view>
#include <memory>

// Forward declarations
namespace expression_parser {
    class ExpressionParser;
}

class Mercedes190ESpeedometer : public QWidget
{
    Q_OBJECT
public:
    using config_t = Mercedes190ESpeedometerConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_speedometer";

    explicit Mercedes190ESpeedometer(const config_t& cfg, QWidget *parent = nullptr);

    void setSpeed(float speed); // Assume input speed is in MPH for this widget
    void setOdometerValue(int value); // Setter for odometer
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onSpeedEvaluated(float mph);
    void onOdometerEvaluated(int miles);

private:
    // Speedometer properties.
    static constexpr float kAngleMinDeg = 210.0f;
    static constexpr float kAngleSweepDeg = -240.0f;

    // Needle properties (Orange, tapered)
    static constexpr QColor kNeedleColor = QColor(255, 165, 0); // Orange
    static constexpr float kNeedleLength = 85.0f;      // Length from pivot to tip
    static constexpr float kNeedleBaseWidth = 4.0f;    // Width at the pivot
    static constexpr float kNeedleTipWidth = 2.0f;     // Width at the tip (can be 0 for a sharp point)

    // Pivot properties.
    static constexpr float kPivotRadius = 8.0f; // Larger pivot as per image
    static constexpr QColor kPivotColor = QColor(40, 40, 40); // Dark grey/black

    // Odometer properties.
    static constexpr uint8_t kNumDigits = 6;
    static constexpr float kDigitWidth = 12.0f;
    static constexpr float kDigitHeight = 18.0f; 
    static constexpr float kDigitSpacing = 0.0f; 

    // Box properties.
    static constexpr float kBoxMarkerRadius = 67.5f; // Radius for the center of the boxes
    static constexpr float kMarkerBoxSquareSize = 2.0f;  // Length of each side of the square.
    static constexpr float kBoxSpacing = 1.0f;      // Spacing between multiple boxes

    // Arc properties.
    static constexpr float kArcRadius = 70.0f;
    static constexpr float kArcThickness = 1.25f;
    static constexpr float kArcNumTextRadius = 85.0f; 
    static constexpr float kMajorTickLen = 6.0f; 
    static constexpr float kMinorTickLen = 4.0f; 

    // KMH Arc properties.
    static constexpr float kKmhArcRadius = 65.0f;
    static constexpr float kKmhArcThickness = 1.25f;
    static constexpr float kKmhArcNumTextRadius = 50.0f; 
    static constexpr float kKmhMajorTickLen = 6.0f;
    static constexpr float kKmhMinorTickLen = 3.0f;

    void drawBackground(QPainter *painter);
    void drawMphTicksAndNumbers(QPainter *painter);
    void drawKmhTicksAndNumbers(QPainter *painter);
    void drawBoxesAtMPH(QPainter *painter, float mphValue, int numBoxes);
    void drawNeedle(QPainter *painter);
    void drawOverlayText(QPainter *painter); // For "miles", "km/h mph" stack etc.
    void drawOdometer(QPainter *painter); // New method for odometer

    float valueToAngle(float value, float maxVal); // Changed to float

    float current_speed_mph_;

    config_t cfg_;

    int odometer_value_; // Stores the odometer reading

    QFont odo_font_;
    QFont mph_font_;
    QFont kmh_font_;

    QFont miles_font_;
    QFont kmh_text_font_;
    QFont unit_font_;
    QFont vdo_font_;
    
    // Zenoh-related members
    std::shared_ptr<zenoh::Session> zenoh_session_;
    
    // Expression parsers for speed and odometer calculations
    std::unique_ptr<expression_parser::ExpressionParser> speed_expression_parser_;
    std::unique_ptr<expression_parser::ExpressionParser> odometer_expression_parser_;
    
    void configureParserSubscriptions();
};

#endif // SPEEDOMETERWIDGETMPH_H 