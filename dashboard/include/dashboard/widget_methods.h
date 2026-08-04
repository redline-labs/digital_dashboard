#ifndef DASHBOARD_WIDGET_METHODS_H_
#define DASHBOARD_WIDGET_METHODS_H_

#include "agent_control/server.h"
#include "dashboard/app_config.h"

#include <QWidget>

#include <functional>

namespace dashboard::agent
{

// How the host application puts a changed config back onto a live widget.
//
// The two apps do this differently and neither way generalises: the editor
// rebuilds the child inside its SelectionFrame, while the dashboard has to
// recreate the widget through widget_factory and re-apply its geometry. So the
// shared code extracts and validates the config and the app supplies the last
// step.
//
// Returns false if the widget cannot be rebuilt, which becomes an
// UNSUPPORTED_IN_APP error rather than a silent no-op.
using ConfigApplier = std::function<bool(QWidget* target, const widget_config_t& config)>;

// Registers widget.describe_config, widget.get_config and widget.set_config.
// Pass a null applier to register the two read-only methods only.
void registerWidgetMethods(agent_control::AgentServer& server, ConfigApplier applier);

// Resolves an addressed widget to the dashboard widget carrying the config.
// In the editor a selector usually lands on the SelectionFrame wrapper, so this
// looks one level down when the target itself is not a known widget type.
QWidget* configBearingWidget(QWidget* target);

}  // namespace dashboard::agent

#endif  // DASHBOARD_WIDGET_METHODS_H_
