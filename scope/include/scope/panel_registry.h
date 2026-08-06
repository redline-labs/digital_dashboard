#ifndef SCOPE_PANEL_REGISTRY_H_
#define SCOPE_PANEL_REGISTRY_H_

// Everything derived from SCOPE_PANEL_TABLE.
//
// ADDING A PANEL TYPE is three steps, and this comment is the canonical
// description of them:
//
//   1. Write the panel under scope/panels/<name>/, with a config struct
//      declared via REFLECT_STRUCT in its own config.h. The class must expose:
//          using config_t = <Name>PanelConfig_t;
//          static constexpr panel_type_t kPanelType = panel_type_t::<enum_name>;
//          static constexpr std::string_view kFriendlyName = "...";
//          const config_t& getConfig() const;
//      and derive from scope::Panel, whose acceptsBinding() is what decides
//      which browser candidates it will take.
//
//   2. Add one line to SCOPE_PANEL_TABLE in scope/panel_table.h.
//
//   3. Add its .cpp and its Q_OBJECT header to scope_core's sources in
//      scope/CMakeLists.txt, and its include/ to the include directories.
//
// Everything else follows: the enum, the config variant, default_panel_config(),
// the Panels menu, the YAML decoder, and what the agent interface will accept.
// The same arrangement as dashboard/include/editor/widget_registry.h, for the
// same reason -- a list repeated in several places is a list that will disagree
// with itself.

#include "scope/panel.h"
#include "scope/panel_table.h"
#include "scope/panel_types.h"

#include "time_series/time_series_panel.h"

#include <memory>
#include <string_view>
#include <variant>
#include <vector>

namespace scope
{

// A panel's configuration, whichever kind it is. std::monostate is the
// "unknown panel type" state, and absorbs the trailing comma from the macro.
#define SCOPE_PANEL_CONFIG_ALT(enum_name, panel_class) panel_class::config_t,
using panel_config_variant_t =
    std::variant<SCOPE_PANEL_TABLE(SCOPE_PANEL_CONFIG_ALT) std::monostate>;
#undef SCOPE_PANEL_CONFIG_ALT

// The enumerator count has to match the table plus `unknown`. Hand-adding an
// enumerator without a table entry would compile and then quietly fail to
// construct, decode or appear in the menu.
#define SCOPE_PANEL_COUNTER(enum_name, panel_class) +1
static_assert(reflection::enum_traits<panel_type_t>::names().size() ==
                  static_cast<std::size_t>(0 SCOPE_PANEL_TABLE(SCOPE_PANEL_COUNTER) + 1),
              "panel_type_t has an enumerator that is not in SCOPE_PANEL_TABLE (or vice versa). "
              "Add the panel to the table rather than to the enum.");
#undef SCOPE_PANEL_COUNTER

// A default-constructed config of the right kind for `type`, or monostate when
// the type is unknown.
inline panel_config_variant_t default_panel_config(panel_type_t type)
{
    panel_config_variant_t config{std::monostate{}};
    switch (type)
    {
#define SCOPE_PANEL_DEFAULT_CASE(enum_name, panel_class) \
    case panel_class::kPanelType:                        \
        config = typename panel_class::config_t{};       \
        break;

        SCOPE_PANEL_TABLE(SCOPE_PANEL_DEFAULT_CASE)
#undef SCOPE_PANEL_DEFAULT_CASE

        case panel_type_t::unknown:
            break;
    }
    return config;
}

// What the Panels menu and the agent interface list.
struct PanelTypeInfo
{
    panel_type_t type;
    std::string_view name;
    std::string_view friendly_name;
};

inline std::vector<PanelTypeInfo> availablePanelTypes()
{
    return {
#define SCOPE_PANEL_INFO(enum_name, panel_class)                                  \
    PanelTypeInfo{panel_class::kPanelType,                                        \
                  reflection::enum_traits<panel_type_t>::to_string(               \
                      panel_class::kPanelType),                                   \
                  panel_class::kFriendlyName},

        SCOPE_PANEL_TABLE(SCOPE_PANEL_INFO)
#undef SCOPE_PANEL_INFO
    };
}

class DataSource;

// Construct a panel from a config variant. Returns nullptr for monostate --
// an unknown `type:` in a workspace -- rather than substituting some other
// panel, because a workspace that silently grows a panel it does not name is
// worse than one that comes back a panel short and says so.
//
// Runs the ADL-found validate() hook before construction, exactly as
// dashboard's widget_factory does, so a panel never sees a config the loader
// would have clamped.
//
// `history_seconds` is the workspace's retention, passed at construction rather
// than set afterwards because a panel builds its buffers while binding and
// changing it later throws away whatever they had already collected.
std::unique_ptr<Panel> createPanel(const panel_config_variant_t& config,
                                   DataSource& source,
                                   double history_seconds =
                                       TimeSeriesPanel::kDefaultHistorySeconds,
                                   QWidget* parent = nullptr);

// The panel type a config variant holds, for saving a workspace back out.
panel_type_t panelTypeOf(const panel_config_variant_t& config);

}  // namespace scope

#endif  // SCOPE_PANEL_REGISTRY_H_
