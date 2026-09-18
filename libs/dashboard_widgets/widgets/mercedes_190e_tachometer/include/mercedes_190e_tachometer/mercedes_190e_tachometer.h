#ifndef TACHOMETERWIDGET_H
#define TACHOMETERWIDGET_H


#include <vector>
#include "mercedes_190e_tachometer/config.h"
#include "qt_helpers/cached_paint_widget.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QString>
#include <QPointF>
#include <QFont>
#include <QColor>
#include <QTimer>
#include <QTime>
#include <QPixmap>

#include <string_view>
#include <memory>

class QPainter;

// Forward declarations
#include "dashboard/expression_subscription.h"

class Mercedes190ETachometer : public qt_helpers::CachedPaintWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190ETachometerConfig_t;
    static constexpr std::string_view kFriendlyName = "Mercedes 190E Tachometer";
    static constexpr widget_type_t kWidgetType = widget_type_t::mercedes_190e_tachometer;

    explicit Mercedes190ETachometer(Mercedes190ETachometerConfig_t cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }

    void setRpm(float rpm); // Expects RPM value e.g., 0 to 7000

    // The needle goes away while the stream is quiet; the clock, which does not
    // come off the bus, keeps running.
    float getRpm() const;

protected:
    void applyPaintTransform(QPainter& painter) const override;
    void paintStaticUnderlay(QPainter& painter) override;
    void paintDynamic(QPainter& painter) override;

private:
    float valueToAngle(float value) const;

    void drawScaleAndNumbers(QPainter *painter);
    void drawRedZone(QPainter *painter);
    void drawStaticText(QPainter *painter);
    void drawNeedle(QPainter *painter);
    void drawClockFace(QPainter *painter);
    void drawClockHands(QPainter *painter);

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
    QFont m_clockFont;

    // Clock specific members
    QTime m_currentTime;
    QTimer *m_clockUpdateTimer;

    void updateClockTime();

    // Expression parser for RPM calculation
    dashboard::ExpressionSubscriptionPtr<float> rpm_expression_parser_;

    // No needle at all while the stream is quiet: a needle parked at zero is a
    // reading, and this dial has no other way to say it has none.
    // The subscription is the only place this is recorded, so there is
    // nothing here to keep in step with it.
    bool rpmStale() const { return rpm_expression_parser_ && rpm_expression_parser_->isStale(); }
};

#endif // TACHOMETERWIDGET_H 