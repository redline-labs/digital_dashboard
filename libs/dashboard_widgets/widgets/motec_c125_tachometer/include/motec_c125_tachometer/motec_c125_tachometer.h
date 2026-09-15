#ifndef MOTEC_C125_TACHOMETER_H
#define MOTEC_C125_TACHOMETER_H

#include "dashboard/stale_aware.h"

#include <vector>
#include "motec_c125_tachometer/config.h"
#include "qt_helpers/cached_paint_widget.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

#include "dashboard/expression_subscription.h"

class QPainter;

class MotecC125Tachometer : public qt_helpers::CachedPaintWidget, public dashboard::StaleAware
{
    Q_OBJECT

public:
    using config_t = MotecC125TachometerConfig_t;
    static constexpr std::string_view kFriendlyName = "MoTeC C125 Tachometer";
    static constexpr widget_type_t kWidgetType = widget_type_t::motec_c125_tachometer;

    explicit MotecC125Tachometer(const MotecC125TachometerConfig_t& cfg, QWidget* parent = nullptr);
    const config_t& getConfig() const { return _cfg; }

    void setRpm(float rpm);

    // The yellow value arc is what this dial says; with no stream there is
    // nothing to fill, so the channel is left empty.
    void setRpmStale(bool stale);

    std::vector<std::string_view> staleBindings() const override { return {"rpm"}; }
    void setBindingStale(std::string_view binding, bool stale) override;
    bool isBindingStale(std::string_view binding) const override;

protected:
    void applyPaintTransform(QPainter& painter) const override;
    void paintStaticUnderlay(QPainter& painter) override;  // backdrop + white base arc
    void paintDynamic(QPainter& painter) override;         // yellow value arc
    void paintStaticOverlay(QPainter& painter) override;   // rings, ticks, digit, redline
    bool hasStaticOverlay() const override { return true; }

private:
    void drawDial(QPainter* painter);
    void drawBackdrop(QPainter* painter);
    void drawBaseArc(QPainter* painter);
    void drawValueArc(QPainter* painter);
    void drawRedline(QPainter* painter);
    void drawTicks(QPainter* painter);
    void drawCenterDigit(QPainter* painter);
    void drawPageBanner(QPainter* painter);

    MotecC125TachometerConfig_t _cfg;
    float _rpm; // current rpm
    bool _rpm_stale = false;

    // Fonts
    QFont _digitFont;

    // Optional live data support
    dashboard::ExpressionSubscriptionPtr<float> _expression_parser;
};

#endif // MOTEC_C125_TACHOMETER_H



