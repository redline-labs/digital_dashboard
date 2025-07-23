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

class Mercedes190EClusterGauge : public QWidget
{
    Q_OBJECT
public:
    using config_t = Mercedes190EClusterGaugeConfig_t;

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
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_H 