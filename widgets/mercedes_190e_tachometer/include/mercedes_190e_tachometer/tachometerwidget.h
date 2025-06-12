#ifndef TACHOMETERWIDGET_H
#define TACHOMETERWIDGET_H

#include <QWidget>
#include <QString>
#include <QPointF>
#include <QFont>
#include <QColor>
#include <QTimer>
#include <QTime>

class QPainter;

class TachometerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TachometerWidget(QWidget *parent = nullptr);

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
    const float m_maxRpmDisplay; // Max value on dial (e.g., 70 for 7000 RPM)
    const float m_angleStart_deg;    // Angle for 0 RPM
    const float m_angleSweep_deg;    // Total sweep angle for m_maxRpmDisplay

    const float m_scaleRadius;       // Radius for the tick marks
    const float m_numberRadius;      // Radius for the numbers
    const float m_textLabelRadius;   // Radius for "x100 1/min" text (approximate)

    const float m_pivotRadius;       // Radius of the central pivot hole/dot
    const float m_needleLength;      // Length of the needle from pivot

    // Red Zone parameters (values on 0-70 scale)
    const float m_redZone1_StartValue;
    const float m_redZone1_EndValue;
    const float m_redZoneArcWidth;

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