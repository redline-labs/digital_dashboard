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

#include <string_view>

namespace expression_parser {
    class ExpressionParser;
}

class SparklineItem : public QWidget {
    Q_OBJECT

public:
    using config_t = SparklineConfig_t;
    static constexpr std::string_view kWidgetName = "sparkline";

    explicit SparklineItem(const SparklineConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }
    void addDataPoint(double value);
    void setYAxisRange(double minVal, double maxVal);
    
    // Set Zenoh session for data subscription
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void forceRepaint();
    void onDataEvaluated(double value);

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
    
    // Expression parser owned subscription if configured
    std::unique_ptr<expression_parser::ExpressionParser> _expression_parser;

    // Fallback direct subscription when schema/expression not provided
    std::unique_ptr<zenoh::Subscriber<void>> _zenoh_subscriber;
};

#endif // SPARKLINEITEM_H 