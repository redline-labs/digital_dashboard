#ifndef CENTER_BAR_WIDGET_H
#define CENTER_BAR_WIDGET_H

#include "center_bar/config.h"
#include "dashboard/widget_types.h"
#include "dashboard/expression_subscription.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

class QPainter;

class CenterBarWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = CenterBarConfig_t;
    static constexpr std::string_view kFriendlyName = "Center Bar";
    static constexpr widget_type_t kWidgetType = widget_type_t::center_bar;

    explicit CenterBarWidget(const CenterBarConfig_t& cfg, QWidget* parent = nullptr);
    const config_t& getConfig() const { return _cfg; }

  public slots:
    void setValue(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CenterBarConfig_t _cfg;
    double _value = 0.0;

    QFont _label_font;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // CENTER_BAR_WIDGET_H
