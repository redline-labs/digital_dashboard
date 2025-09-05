#ifndef MERCEDES_190E_CLUSTER_GAUGE_H
#define MERCEDES_190E_CLUSTER_GAUGE_H

#include "mercedes_190e_cluster_gauge/config.h"

#include <QWidget>
#include <QPainter>
#include <QPointF>
#include <QtMath>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QFontDatabase>
#include <QSvgRenderer>

#include <string_view>
#include <memory>

// Forward declarations
namespace zenoh_subscriber {
    class ZenohSubscriber;
}

class Mercedes190EClusterGauge : public QWidget
{
    Q_OBJECT
public:
    using config_t = Mercedes190EClusterGaugeConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_cluster_gauge";

    explicit Mercedes190EClusterGauge(const Mercedes190EClusterGaugeConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return m_config; }

private slots:
    // Setters for each sub-gauge value
    void setFuelGaugeValue(float value);
    void setOilPressureGaugeValue(float value);
    void setCoolantTemperatureGaugeValue(float value);
    void setEconomyGaugeValue(float value);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawBackground(QPainter *painter);
    void drawFuelGauge(QPainter *painter, const sub_gauge_config_t& gauge, 
                       float centerX, float centerY);
    void drawOilPressureGauge(QPainter *painter, const sub_gauge_config_t& gauge,
                              float centerX, float centerY);
    void drawCoolantTemperatureGauge(QPainter *painter, const sub_gauge_config_t& gauge,
                                     float centerX, float centerY);
    
    float valueToAngle(float value, float minVal, float maxVal);

    Mercedes190EClusterGaugeConfig_t m_config;
    QString m_fontFamily; // Font family for text rendering

    // Fuel gauge current value
    float fuel_gauge_current_value_;

    // Oil pressure gauge current value
    float oil_pressure_gauge_current_value_;

    // Coolant temperature gauge current value
    float coolant_temperature_gauge_current_value_;

    // Economy gauge current value
    float economy_gauge_current_value_;

    // Expression parsers for each sub-gauge
    std::unique_ptr<zenoh_subscriber::ZenohSubscriber> top_gauge_expression_parser_;
    std::unique_ptr<zenoh_subscriber::ZenohSubscriber> right_gauge_expression_parser_;
    std::unique_ptr<zenoh_subscriber::ZenohSubscriber> bottom_gauge_expression_parser_;
    std::unique_ptr<zenoh_subscriber::ZenohSubscriber> left_gauge_expression_parser_;

    QSvgRenderer fuel_icon_svg_renderer_;
    QSvgRenderer oil_icon_svg_renderer_;
    QSvgRenderer coolant_icon_svg_renderer_;
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_H 