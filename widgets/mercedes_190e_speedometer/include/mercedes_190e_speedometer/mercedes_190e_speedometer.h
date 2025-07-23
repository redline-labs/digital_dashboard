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
#include <QFontDatabase>

class Mercedes190ESpeedometer : public QWidget
{
    Q_OBJECT
public:
    using config_t = Mercedes190ESpeedometerConfig_t;

    explicit Mercedes190ESpeedometer(const config_t& cfg, QWidget *parent = nullptr);
    void setSpeed(float speed); // Assume input speed is in MPH for this widget
    float getSpeed() const;
    void setOdometerValue(int value); // Setter for odometer
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onSpeedDataReceived(double speedMps);

private:
    void drawBackground(QPainter *painter);
    void drawMphTicksAndNumbers(QPainter *painter);
    void drawKmhTicksAndNumbers(QPainter *painter);
    void drawNeedle(QPainter *painter);
    void drawOverlayText(QPainter *painter); // For "miles", "km/h mph" stack etc.
    void drawOdometer(QPainter *painter); // New method for odometer

    float valueToAngle(float value, float maxVal); // Changed to float

    float m_currentSpeedMph;

    config_t _cfg;

    // Angles are Qt standard (0 at 3 o'clock, positive CCW), assuming Y-up due to painter.scale(1,-1)
    const float m_angleValueMin = 210.0f; // Changed to float
    const float m_angleSweep = -240.0f;   // Changed to float
                                          // So 120 MPH is at 210 - 240 = -30 deg (approx 5 o'clock)

    QString m_fontFamily; // Added to store Futura font family
    int m_odometerValue; // Stores the odometer reading
    
    // Zenoh-related members
    std::shared_ptr<zenoh::Session> _zenoh_session;
    std::unique_ptr<zenoh::Subscriber<void>> _zenoh_subscriber;
    
    void createZenohSubscription();
};

#endif // SPEEDOMETERWIDGETMPH_H 