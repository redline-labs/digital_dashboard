#ifndef BATTERYTELLTALEWIDGET_H
#define BATTERYTELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>
#include <memory>

// Forward declarations
class QSvgRenderer;

namespace expression_parser {
    class ExpressionParser;
}


class Mercedes190EBatteryTelltale : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190EBatteryTelltaleConfig_t;
    static constexpr std::string_view kWidgetName = "mercedes_190e_battery_telltale";

    explicit Mercedes190EBatteryTelltale(const Mercedes190EBatteryTelltaleConfig_t& cfg, QWidget *parent = nullptr);
    ~Mercedes190EBatteryTelltale();

    void setAsserted(bool asserted);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onConditionEvaluated(bool asserted);

private:
    void updateColors();

    Mercedes190EBatteryTelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QColor mBackgroundColor;
    QColor mIconColor;
    
    // Colors
    static constexpr QColor kAssertedBackground = QColor(200, 50, 50);    // Medium red
    static constexpr QColor kNormalBackground = QColor(60, 60, 60);       // Dark gray
    static constexpr QColor kAssertedIcon = QColor(255, 255, 255);        // White when asserted
    static constexpr QColor kNormalIcon = QColor(120, 120, 120);          // Light gray when normal

    // Expression parser for condition evaluation
    std::unique_ptr<expression_parser::ExpressionParser> _expression_parser;
};

#endif // BATTERYTELLTALEWIDGET_H 