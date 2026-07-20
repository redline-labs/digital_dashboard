#ifndef TELLTALEWIDGET_H
#define TELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>
#include "dashboard/cached_paint_widget.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include <string_view>
#include <map>
#include <memory>

// Forward declarations
class QSvgRenderer;

namespace pub_sub { class ZenohExpressionSubscriber; }


class Mercedes190ETelltale : public dashboard::CachedPaintWidget
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
    // The whole telltale (background, border, tinted icon) only changes with
    // size or asserted state, so it is all cached static content.
    void paintStaticUnderlay(QPainter& painter) override;
    void paintDynamic(QPainter& painter) override;

private:
    void updateColors();

    Mercedes190ETelltaleConfig_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QColor mBackgroundColor;
    QColor mIconColor;
    QString mSvgAlias;

    // Expression parser for condition evaluation
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> _expression_parser;
};

#endif // TELLTALEWIDGET_H


