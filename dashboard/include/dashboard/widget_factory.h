#ifndef DASHBOARD_WIDGET_FACTORY_H
#define DASHBOARD_WIDGET_FACTORY_H

#include "dashboard/app_config.h"
#include "dashboard/config_limits.h"

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <spdlog/spdlog.h>

namespace widget_factory
{

// Detects the optional validate() hook described in dashboard/config_limits.h.
// Found by ADL, so a config declares it as a free function next to the struct
// and REFLECT_STRUCT does not have to know about it.
template <typename Cfg>
concept HasValidate = requires(Cfg& cfg) {
    { validate(cfg) } -> std::same_as<std::vector<std::string>>;
};

// Runs a config's range checking, if it has any, and logs what it changed.
// Called on the way to construction, so a widget never sees a config it cannot
// draw -- a max of zero to divide by, an inverted range, a list long enough to
// stall a paint.
template <typename Cfg>
void applyLimits(Cfg& cfg, std::string_view widget_type)
{
    if constexpr (HasValidate<Cfg>)
    {
        for (const std::string& note : validate(cfg))
        {
            SPDLOG_WARN("{}: {}.", widget_type, note);
        }
    }
}

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

            // Copy, because clamping has to happen before the widget reads the
            // config and the caller's copy is const.
            cfg_t checked = cfg;
            applyLimits(checked, reflection::enum_to_string(traits::type));

            widget = new widget_t(checked, parent);
        }
    }, widget_config.config);

    return widget;
}

} // namespace widget_factory

#endif // DASHBOARD_WIDGET_FACTORY_H
