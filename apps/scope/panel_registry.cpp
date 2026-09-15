#include "scope/panel_registry.h"

#include "scope/data_source.h"

#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>

namespace scope
{

namespace
{

// The ADL-found validate(cfg) hook a panel config may declare, same contract as
// the dashboard widgets'. Optional, so a config with nothing worth clamping
// declares nothing.
template <typename Cfg>
concept HasValidate = requires(Cfg& cfg) {
    { validate(cfg) } -> std::same_as<std::vector<std::string>>;
};

template <typename Cfg>
void applyLimits(Cfg& cfg, std::string_view panel_type)
{
    if constexpr (HasValidate<Cfg>)
    {
        for (const std::string& note : validate(cfg))
        {
            SPDLOG_WARN("{}: {}.", panel_type, note);
        }
    }
}

}  // namespace

panel_config_variant_t clampPanelConfig(panel_config_variant_t config)
{
    std::visit(
        [](auto& cfg)
        {
            using cfg_t = std::decay_t<decltype(cfg)>;
            if constexpr (!std::is_same_v<cfg_t, std::monostate>)
            {
#define SCOPE_PANEL_CLAMP(enum_name, panel_class)                                  \
    if constexpr (std::is_same_v<cfg_t, typename panel_class::config_t>)           \
    {                                                                              \
        applyLimits(cfg, reflection::enum_traits<panel_type_t>::to_string(         \
                             panel_class::kPanelType));                            \
    }
                SCOPE_PANEL_TABLE(SCOPE_PANEL_CLAMP)
#undef SCOPE_PANEL_CLAMP
            }
        },
        config);
    return config;
}

std::unique_ptr<Panel> createPanel(const panel_config_variant_t& config,
                                   DataSource& source,
                                   double history_seconds,
                                   QWidget* parent)
{
    std::unique_ptr<Panel> panel;

    std::visit(
        [&](const auto& cfg) {
            using cfg_t = std::decay_t<decltype(cfg)>;
            if constexpr (std::is_same_v<cfg_t, std::monostate>)
            {
                // Unknown panel type; nothing to construct. The caller reports
                // it -- a workspace that comes back a panel short and says so
                // beats one that silently grows a panel it does not name.
            }
            else
            {
#define SCOPE_PANEL_CONSTRUCT(enum_name, panel_class)                                    \
    if constexpr (std::is_same_v<cfg_t, typename panel_class::config_t>)                 \
    {                                                                                    \
        /* Clamp before construction, never after: a panel that had to be told its */    \
        /* own config was out of range would report the unclamped values back when */    \
        /* the workspace was saved. Same rule as widget_factory. */                      \
        cfg_t checked = cfg;                                                             \
        applyLimits(checked, reflection::enum_traits<panel_type_t>::to_string(           \
                                 panel_class::kPanelType));                              \
        panel = std::make_unique<panel_class>(checked, source, history_seconds, parent);  \
    }

                SCOPE_PANEL_TABLE(SCOPE_PANEL_CONSTRUCT)
#undef SCOPE_PANEL_CONSTRUCT
            }
        },
        config);

    return panel;
}

panel_type_t panelTypeOf(const panel_config_variant_t& config)
{
    panel_type_t type = panel_type_t::unknown;

    std::visit(
        [&](const auto& cfg) {
            using cfg_t = std::decay_t<decltype(cfg)>;
            if constexpr (!std::is_same_v<cfg_t, std::monostate>)
            {
#define SCOPE_PANEL_TYPE_OF(enum_name, panel_class)                          \
    if constexpr (std::is_same_v<cfg_t, typename panel_class::config_t>)     \
    {                                                                        \
        type = panel_class::kPanelType;                                      \
    }

                SCOPE_PANEL_TABLE(SCOPE_PANEL_TYPE_OF)
#undef SCOPE_PANEL_TYPE_OF
            }
        },
        config);

    return type;
}

}  // namespace scope
