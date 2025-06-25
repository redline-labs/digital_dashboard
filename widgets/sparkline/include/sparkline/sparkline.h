#ifndef SPARKLINEITEM_H
#define SPARKLINEITEM_H

#include "sparkline/config.h"

#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QVector>
#include <QLabel>
#include <QVBoxLayout>

class SparklineItem : public QWidget {
    Q_OBJECT

public:
    explicit SparklineItem(const sparkline_config_t& cfg, QWidget *parent = nullptr);
    void addDataPoint(double value);
    void setYAxisRange(double minVal, double maxVal);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void forceRepaint();

private:
    sparkline_config_t _cfg;

    QVector<double> dataPoints;
    QLabel *valueLabel;
    QLabel *unitsLabel;
    QTimer *m_repaintTimer;
    double m_lastValue;
    static const int MAX_DATA_POINTS = 100; // Max points to display in sparkline
};

#endif // SPARKLINEITEM_H 