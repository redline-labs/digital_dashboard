#ifndef TACHOMETERWIDGET_H
#define TACHOMETERWIDGET_H

#include "mercedes_190e_tachometer/config.h"
#include "zenoh.hxx"

#include <QWidget>
#include <QString>
#include <QPointF>
#include <QFont>
#include <QColor>
#include <QTimer>
#include <QTime>

#include <string_view>
#include <memory>

class QPainter;

// Forward declarations
namespace expression_parser {
    class ExpressionParser;
}

class Mercedes190ETachometer : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190ETachometerConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_tachometer";

    explicit Mercedes190ETachometer(Mercedes190ETachometerConfig_t cfg, QWidget *parent = nullptr);

    void setRpm(float rpm); // Expects RPM value e.g., 0 to 7000
    float getRpm() const;
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onRpmEvaluated(float rpm);

private:
    float valueToAngle(float value) const;

    void drawBackground(QPainter *painter);
    void drawScaleAndNumbers(QPainter *painter);
    void drawRedZone(QPainter *painter);
    void drawStaticText(QPainter *painter);
    void drawNeedle(QPainter *painter);
    void drawClock(QPainter *painter);

    float m_currentRpmValue; // Stores value on 0-70 scale for drawing

    // Drawing parameters based on the new reference image
    const float m_angleStart_deg;    // Angle for 0 RPM
    const float m_angleSweep_deg;    // Total sweep angle for m_maxRpmDisplay

    const float m_scaleRadius;       // Radius for the tick marks
    const float m_numberRadius;      // Radius for the numbers

    const float m_pivotRadius;       // Radius of the central pivot hole/dot
    const float m_needleLength;      // Length of the needle from pivot

    // Red Zone parameters (values on 0-70 scale)
    const float m_redZoneArcWidth;

    Mercedes190ETachometerConfig_t _cfg;

    QString m_fontFamily;
    QFont m_dialFont;
    QFont m_labelFont;

    // Clock specific members
    QTime m_currentTime;
    QTimer *m_clockUpdateTimer;

    void updateClockTime();
    
    // Zenoh-related members
    std::shared_ptr<zenoh::Session> _zenoh_session;
    
    // Expression parser for RPM calculation
    std::unique_ptr<expression_parser::ExpressionParser> rpm_expression_parser_;
};

#endif // TACHOMETERWIDGET_H 