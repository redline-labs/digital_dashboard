#ifndef TELLTALEWIDGET_H
#define TELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>
#include "qt_helpers/cached_paint_widget.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>
#include <memory>
#include <vector>

#include "dashboard/stale_aware.h"

// Forward declarations
class QSvgRenderer;

#include "dashboard/expression_subscription.h"


class Mercedes190ETelltale : public qt_helpers::CachedPaintWidget, public dashboard::StaleAware
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

    // A third state, distinct from both on and off: a lamp that is dark because
    // nothing is reporting looks exactly like one that is dark because the
    // condition is false, and those mean opposite things.
    void setConditionStale(bool stale);

    std::vector<std::string_view> staleBindings() const override { return {"condition"}; }
    void setBindingStale(std::string_view binding, bool stale) override;
    bool isBindingStale(std::string_view binding) const override;

    QSize sizeHint() const override;

protected:
    // The whole telltale (background, border, tinted icon) only changes with
    // size or asserted state, so it is all cached static content.
    void paintStaticUnderlay(QPainter& painter) override;
    void paintDynamic(QPainter& painter) override;

private:
    void updateColors();

    Mercedes190ETelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    bool mStale = false;
    QColor mBackgroundColor;
    QColor mIconColor;
    QString mSvgAlias;

    // Expression parser for condition evaluation
    dashboard::ExpressionSubscriptionPtr<bool> _expression_parser;
};

#endif // TELLTALEWIDGET_H


