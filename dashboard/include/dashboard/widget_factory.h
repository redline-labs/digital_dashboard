#ifndef DASHBOARD_WIDGET_FACTORY_H
#define DASHBOARD_WIDGET_FACTORY_H

#include "dashboard/app_config.h"

#include <type_traits>
#include <spdlog/spdlog.h>

namespace widget_factory
{

inline QWidget* createWidgetFromConfig(const widget_config_t& widget_config, QWidget* parent)
{
    if (widget_config.type == widget_type_t::unknown)
    {
        return nullptr;
    }

    QWidget* widget = nullptr;
    std::visit([&](const auto& cfg) {
        using cfg_t = std::decay_t<decltype(cfg)>;
        if constexpr (std::is_same_v<cfg_t, std::monostate>)
        {
            // Unknown widget type; nothing to construct.
        }
        else
        {
            using traits = widget_registry::config_traits<cfg_t>;
            using widget_t = typename traits::widget_t;

            if (widget_config.type != traits::type)
            {
                SPDLOG_WARN("Widget config type mismatch: expected '{}', got '{}'",
                            reflection::enum_to_string(traits::type),
                            reflection::enum_to_string(widget_config.type));
                return;
            }

            widget = new widget_t(cfg, parent);
        }
    }, widget_config.config);

    return widget;
}

} // namespace widget_factory

#endif // DASHBOARD_WIDGET_FACTORY_H
