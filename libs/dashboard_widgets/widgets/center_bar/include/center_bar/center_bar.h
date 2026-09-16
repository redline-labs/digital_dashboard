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

  public:
    // The marker is hidden rather than parked at centre: centre means zero
    // gain, which is a reading, and a strip cannot say "no data" any other way.

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CenterBarConfig_t _cfg;
    double _value = 0.0;

    QFont _label_font;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;

    // The marker goes away and the track greys while the stream is quiet: a
    // marker parked at centre is a reading.
    // The subscription is the only place this is recorded, so there is
    // nothing here to keep in step with it.
    bool valueStale() const { return _expression_parser && _expression_parser->isStale(); }
};

#endif // CENTER_BAR_WIDGET_H
