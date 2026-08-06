#ifndef SCOPE_WORKSPACE_H_
#define SCOPE_WORKSPACE_H_

#include "scope/panel_registry.h"
#include "scope/panel_types.h"

#include "config_codec/config_validation.h"
#include "config_codec/config_yaml.h"
#include "reflection/reflection.h"

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>
#include <vector>

namespace scope
{

// One panel in a saved workspace.
//
// Not a REFLECT_STRUCT, because the config member is a variant whose active
// alternative is chosen by `type` -- exactly the shape widget_config_t has, and
// it needs the same hand-written YAML conversion for the same reason.
struct panel_entry_t
{
    panel_type_t type = panel_type_t::unknown;

    // The dock's objectName. Load-bearing: restoreState() matches docks by it,
    // so a workspace whose ids do not match its dock_state comes back with the
    // panels present but arranged as if it had never been saved.
    std::string id;

    panel_config_variant_t config{std::monostate{}};
};

bool operator==(const panel_entry_t& lhs, const panel_entry_t& rhs);

REFLECT_STRUCT(scope_workspace_t,
    (std::string, name, "",
        "Name", "Shown in the window title"),
    (double, history_seconds, 300.0,
        "History (s)", "Seconds of samples retained per signal"),
    (double, window_seconds, 30.0,
        "Window (s)", "Seconds of history shown at once"),
    (uint16_t, render_rate_hz, 30,
        "Render Rate (Hz)", "How often panels redraw"),
    (std::vector<panel_entry_t>, panels, {},
        "Panels", "The panels this workspace contains"),
    (std::string, dock_state, "",
        "Dock State", "Base64 QMainWindow::saveState(); arrangement only")
)

std::vector<config_codec::Issue> validate_workspace(const YAML::Node& root);

// Returns nullopt when the file cannot be read or parsed at all. Problems
// inside a file that still parses are reported through validate_workspace and
// do not stop it loading -- a workspace missing one panel is more useful than
// no workspace.
std::optional<scope_workspace_t> load_workspace(const std::string& path);

bool save_workspace(const scope_workspace_t& workspace, const std::string& path);

}  // namespace scope

namespace YAML
{

// More specialized than the generic reflected-struct specialization in
// config_codec/config_yaml.h, so this one wins for panel_entry_t.
template <>
struct convert<scope::panel_entry_t>
{
    static Node encode(const scope::panel_entry_t& rhs);
    static bool decode(const Node& node, scope::panel_entry_t& rhs);
};

}  // namespace YAML

#endif  // SCOPE_WORKSPACE_H_
