#ifndef SPARKLINEITEM_H
#define SPARKLINEITEM_H

#include "sparkline/config.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QVector>
#include <QLabel>
#include <QVBoxLayout>

#include <memory>
#include <string_view>
#include <vector>

#include "dashboard/stale_aware.h"

#include "dashboard/expression_subscription.h"

class SparklineItem : public QWidget, public dashboard::StaleAware {
    Q_OBJECT

public:
    using config_t = SparklineConfig_t;
    static constexpr std::string_view kFriendlyName = "Sparkline";
    static constexpr widget_type_t kWidgetType = widget_type_t::sparkline;

    explicit SparklineItem(const SparklineConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }
    void setLatestValue(double value);

    // The trace stops scrolling and greys. A sparkline that kept shifting in
    // its last value would draw a flat line, which is a reading.
    void setValueStale(bool stale);

    std::vector<std::string_view> staleBindings() const override { return {"value"}; }
    void setBindingStale(std::string_view binding, bool stale) override;
    bool isBindingStale(std::string_view binding) const override;
    void setYAxisRange(double minVal, double maxVal);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void forceRepaint();

private:
    SparklineConfig_t _cfg;

    QVector<double> dataPoints;
    QLabel *valueLabel;
    QLabel *unitsLabel;
    QTimer *m_repaintTimer;
    double m_lastValue;
    QString m_lastValueText;

    // Paint resources derived from config once at construction.
    QColor m_lineColor;
    QColor m_gradientStartColor;
    QColor m_gradientEndColor;
    QPen m_linePen;
    qsizetype m_writeIndex = 0;
    bool m_stale = false;
    static const int MAX_DATA_POINTS = 100; // Max points to display in sparkline

    // Expression parser owned subscription if configured
    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // SPARKLINEITEM_H 