#ifndef TELLTALEWIDGET_H
#define TELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>
#include <memory>
#include <vector>


// Forward declarations
class QSvgRenderer;

#include "dashboard/expression_subscription.h"


class Mercedes190ETelltale : public QWidget
{
    Q_OBJECT

public:
    using config_t = Mercedes190ETelltaleConfig_t;
    static constexpr std::string_view kFriendlyName = "Mercedes 190E Telltale";
    static constexpr widget_type_t kWidgetType = widget_type_t::mercedes_190e_telltale;

    explicit Mercedes190ETelltale(const Mercedes190ETelltaleConfig_t& cfg, QWidget *parent = nullptr);
    const config_t& getConfig() const { return _cfg; }
    ~Mercedes190ETelltale();

    void setAsserted(bool asserted);

    QSize sizeHint() const override;

protected:
    // Painted outright rather than through qt_helpers::CachedPaintWidget. The
    // whole lamp -- background, border, tinted icon -- depends on whether it is
    // lit, which is neither size nor DPR, so it was never static content: it
    // sat in the cached layer and threw that cache away on every change. A
    // telltale repaints only when it lights or goes out, and the paint is a
    // fill, a rect and a 64x64 SVG.
    void paintEvent(QPaintEvent* event) override;

private:
    Mercedes190ETelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QString mSvgAlias;

    // Expression parser for condition evaluation
    dashboard::ExpressionSubscriptionPtr<bool> _expression_parser;

    // Nothing is reporting the condition. The subscription is the only place
    // this is recorded.
    bool conditionStale() const { return _expression_parser && _expression_parser->isStale(); }

    // A warning lamp fails lit. A dark lamp says the condition is false, and
    // saying that when nothing has reported for seconds is the one answer a
    // brake or battery lamp must never give -- so a stream that has stopped
    // lights it, the same as the condition holding would.
    bool lit() const { return mAsserted || conditionStale(); }

    QColor backgroundColor() const;
    QColor iconColor() const;
};

#endif // TELLTALEWIDGET_H


