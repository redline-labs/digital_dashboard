#ifndef SPARKLINEITEM_H
#define SPARKLINEITEM_H

#include "sparkline/config.h"
#include "zenoh.hxx"

#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QVector>
#include <QLabel>
#include <QVBoxLayout>
#include <QMetaObject>
#include <memory>
#include <map>

class SparklineItem : public QWidget {
    Q_OBJECT

public:
    using config_t = SparklineConfig_t;

    explicit SparklineItem(const SparklineConfig_t& cfg, QWidget *parent = nullptr);
    void addDataPoint(double value);
    void setYAxisRange(double minVal, double maxVal);
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void forceRepaint();
    void onDataReceived(double value);

private:
    SparklineConfig_t _cfg;

    QVector<double> dataPoints;
    QLabel *valueLabel;
    QLabel *unitsLabel;
    QTimer *m_repaintTimer;
    double m_lastValue;
    static const int MAX_DATA_POINTS = 100; // Max points to display in sparkline
    
    // Zenoh-related members
    std::shared_ptr<zenoh::Session> _zenoh_session;
    std::unique_ptr<zenoh::Subscriber<void>> _zenoh_subscriber;
    
    void createZenohSubscription();
};

#endif // SPARKLINEITEM_H 