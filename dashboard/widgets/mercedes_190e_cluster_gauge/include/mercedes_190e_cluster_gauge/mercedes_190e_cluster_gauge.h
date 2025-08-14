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

#include <string_view>
#include <memory>

// Forward declarations
namespace expression_parser {
    class ExpressionParser;
}

class Mercedes190EClusterGauge : public QWidget
{
    Q_OBJECT
public:
    using config_t = Mercedes190EClusterGaugeConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_cluster_gauge";

    explicit Mercedes190EClusterGauge(const Mercedes190EClusterGaugeConfig_t& cfg, QWidget *parent = nullptr);
    virtual ~Mercedes190EClusterGauge() = default;
    
    // Setters for each sub-gauge value
    void setTopGaugeValue(float value);
    void setRightGaugeValue(float value);
    void setBottomGaugeValue(float value);
    void setLeftGaugeValue(float value);
    
    // Getters for each sub-gauge value
    float getTopGaugeValue() const;
    float getRightGaugeValue() const;
    float getBottomGaugeValue() const;
    float getLeftGaugeValue() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onTopGaugeEvaluated(float value);
    void onRightGaugeEvaluated(float value);
    void onBottomGaugeEvaluated(float value);
    void onLeftGaugeEvaluated(float value);

private:
    void drawBackground(QPainter *painter);
    void drawSubGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge, 
                      float centerX, float centerY, float startAngle);
    void drawOilPressureGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge,
                              float centerX, float centerY);
    void drawCoolantTemperatureGauge(QPainter *painter, const Mercedes190EClusterGaugeConfig_t::sub_gauge_config_t& gauge,
                                     float centerX, float centerY);
    void drawCenterHole(QPainter *painter, float centerX, float centerY);
    
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
    std::unique_ptr<expression_parser::ExpressionParser> top_gauge_expression_parser_;
    std::unique_ptr<expression_parser::ExpressionParser> right_gauge_expression_parser_;
    std::unique_ptr<expression_parser::ExpressionParser> bottom_gauge_expression_parser_;
    std::unique_ptr<expression_parser::ExpressionParser> left_gauge_expression_parser_;
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_H 