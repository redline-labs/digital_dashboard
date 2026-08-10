#ifndef SCOPE_PANEL_REGISTRY_H_
#define SCOPE_PANEL_REGISTRY_H_

// Everything derived from SCOPE_PANEL_TABLE.
//
// ADDING A PANEL TYPE is three steps, and this comment is the canonical
// description of them:
//
//   1. Write the panel under scope/panels/<name>/, with a config struct
//      declared via REFLECT_STRUCT in its own config.h AND a stats struct
//      declared the same way in its own stats.h. The class must expose:
//          using config_t = <Name>PanelConfig_t;
//          using stats_t  = <Name>PanelStats_t;
//          static constexpr panel_type_t kPanelType = panel_type_t::<enum_name>;
//          static constexpr std::string_view kFriendlyName = "...";
//          static constexpr std::string_view kToolbarGlyph = "...";
//          const config_t& getConfig() const;
//          void applyConfig(const config_t&);
//          stats_t stats() const;
//      and derive from scope::Panel, whose acceptsBinding() is what decides
//      which browser candidates it will take.
//
//      THE STATS STRUCT IS NOT OPTIONAL, and it is what `scope.stats` and
//      `scope.describe_stats` serve without either of them naming a panel type.
//      Put in it what would tell you the panel is lying -- counts of what
//      arrived and what was dropped -- rather than what a screenshot already
//      shows. A panel with genuinely nothing to report declares an empty struct;
//      omitting it is a compile error, by design, because the alternative is an
//      RPC that answers `{}` and looks like a working panel with no data.
//
//      kToolbarGlyph is one or two characters shown on the toolbar's Add
//      button. A glyph rather than an icon because there is no icon pipeline in
//      this tree at all -- no .qrc scope links against, no QIcon anywhere --
//      and inventing one is a larger change than any panel. Swapping glyphs for
//      QIcons later is a change to this field and to the toolbar, and to
//      nothing else.
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

#include "table/table_panel.h"
#include "time_series/time_series_panel.h"
#include "video/video_panel.h"

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

// The same trick for what a panel has RECEIVED, as opposed to what it was
// configured to show. Read-only, and a distinct type from the config variant so
// that applyPanelConfig() cannot be handed one.
//
// This exists so that reporting a panel's state is a property of the table
// rather than a method on the agent interface. `scope.sample_stats` used to
// qobject_cast to TimeSeriesPanel and skip anything else, which meant every new
// panel kind needed its own RPC -- exactly the per-type list SCOPE_PANEL_TABLE
// exists to prevent.
#define SCOPE_PANEL_STATS_ALT(enum_name, panel_class) panel_class::stats_t,
using panel_stats_variant_t =
    std::variant<SCOPE_PANEL_TABLE(SCOPE_PANEL_STATS_ALT) std::monostate>;
#undef SCOPE_PANEL_STATS_ALT

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

    // What the toolbar's Add button shows. See the recipe above.
    std::string_view toolbar_glyph;
};

inline std::vector<PanelTypeInfo> availablePanelTypes()
{
    return {
#define SCOPE_PANEL_INFO(enum_name, panel_class)                                  \
    PanelTypeInfo{panel_class::kPanelType,                                        \
                  reflection::enum_traits<panel_type_t>::to_string(               \
                      panel_class::kPanelType),                                   \
                  panel_class::kFriendlyName,                                     \
                  panel_class::kToolbarGlyph},

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

// ------------------------------------------------- reading a panel generically
//
// FREE FUNCTIONS RATHER THAN VIRTUALS ON Panel, and the reason is an include
// cycle rather than a preference. The variants above are built from the panel
// classes, so this header sits ABOVE them; Panel sits below, because every panel
// header includes it. A `virtual panel_config_variant_t configVariant()` on
// Panel would therefore need a type Panel cannot see.
//
// Dispatching here instead costs a qobject_cast chain -- generated from the same
// table, so it stays exhaustive by construction -- and buys the property that
// matters: a new panel is still ONE line in SCOPE_PANEL_TABLE. Nothing below
// calls these per frame; they serve a workspace save and three RPCs.
//
// The macro expands `p->getConfig()`, `p->applyConfig()` and `p->stats()` for
// every row, so a panel class that omits one fails to COMPILE rather than
// silently reporting nothing. That is the whole enforcement a virtual would
// have given.

// A panel's live configuration, as the variant a workspace stores. monostate
// only if the table and the panel disagree, which cannot compile.
inline panel_config_variant_t panelConfigOf(const Panel& panel)
{
    panel_config_variant_t out{std::monostate{}};

#define SCOPE_PANEL_CONFIG_OF(enum_name, panel_class)               \
    if (const auto* p = qobject_cast<const panel_class*>(&panel))   \
    {                                                               \
        out = p->getConfig();                                       \
    }

    SCOPE_PANEL_TABLE(SCOPE_PANEL_CONFIG_OF)
#undef SCOPE_PANEL_CONFIG_OF

    return out;
}

// Push a configuration back into a panel. False when the variant's alternative
// does not match the panel's kind -- a caller that mixed them up gets a definite
// no rather than a panel that ignored it.
inline bool applyPanelConfig(Panel& panel, const panel_config_variant_t& config)
{
    bool applied = false;

#define SCOPE_PANEL_APPLY_CONFIG(enum_name, panel_class)                       \
    if (auto* p = qobject_cast<panel_class*>(&panel))                          \
    {                                                                          \
        if (const auto* cfg = std::get_if<typename panel_class::config_t>(&config)) \
        {                                                                      \
            p->applyConfig(*cfg);                                              \
            applied = true;                                                    \
        }                                                                      \
    }

    SCOPE_PANEL_TABLE(SCOPE_PANEL_APPLY_CONFIG)
#undef SCOPE_PANEL_APPLY_CONFIG

    return applied;
}

// What a panel has received, as opposed to what it was told to show.
inline panel_stats_variant_t panelStatsOf(const Panel& panel)
{
    panel_stats_variant_t out{std::monostate{}};

#define SCOPE_PANEL_STATS_OF(enum_name, panel_class)                \
    if (const auto* p = qobject_cast<const panel_class*>(&panel))   \
    {                                                               \
        out = p->stats();                                           \
    }

    SCOPE_PANEL_TABLE(SCOPE_PANEL_STATS_OF)
#undef SCOPE_PANEL_STATS_OF

    return out;
}

}  // namespace scope

#endif  // SCOPE_PANEL_REGISTRY_H_
