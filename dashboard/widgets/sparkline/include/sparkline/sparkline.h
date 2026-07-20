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

namespace pub_sub { class ZenohExpressionSubscriber; }

class SparklineItem : public QWidget {
    Q_OBJECT

public:
    using config_t = SparklineConfig_t;
    static constexpr std::string_view kFriendlyName = "Sparkline";
    static constexpr widget_type_t kWidgetType = widget_type_t::sparkline;

    explicit SparklineItem(const SparklineConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }
    void setLatestValue(double value);
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
    int m_writeIndex = 0;
    static const int MAX_DATA_POINTS = 100; // Max points to display in sparkline

    // Expression parser owned subscription if configured
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> _expression_parser;
};

#endif // SPARKLINEITEM_H 