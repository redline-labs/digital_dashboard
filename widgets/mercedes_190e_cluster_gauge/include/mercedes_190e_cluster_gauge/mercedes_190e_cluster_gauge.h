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
    explicit Mercedes190EClusterGauge(const cluster_gauge_config_t& cfg, QWidget *parent = nullptr);
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
    void drawSubGauge(QPainter *painter, const cluster_gauge_config_t::sub_gauge_config_t& gauge, 
                      float centerX, float centerY, float startAngle);
    void drawCenterHole(QPainter *painter, float centerX, float centerY);
    
    float valueToAngle(float value, float minVal, float maxVal);

    cluster_gauge_config_t m_config;
    QString m_fontFamily; // Font family for text rendering
};

#endif // MERCEDES_190E_CLUSTER_GAUGE_H 