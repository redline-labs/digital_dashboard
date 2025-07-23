#ifndef TACHOMETERWIDGET_H
#define TACHOMETERWIDGET_H

#include "mercedes_190e_tachometer/config.h"

#include <QWidget>
#include <QString>
#include <QPointF>
#include <QFont>
#include <QColor>
#include <QTimer>
#include <QTime>

class QPainter;

class Mercedes190ETachometer : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190ETachometerConfig_t;

    explicit Mercedes190ETachometer(Mercedes190ETachometerConfig_t cfg, QWidget *parent = nullptr);

    void setRpm(float rpm); // Expects RPM value e.g., 0 to 7000
    float getRpm() const;

protected:
    void paintEvent(QPaintEvent *event) override;

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

private slots:
    void updateClockTime();
};

#endif // TACHOMETERWIDGET_H 