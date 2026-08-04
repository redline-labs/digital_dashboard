#ifndef AGENT_CONTROL_METHODS_H_
#define AGENT_CONTROL_METHODS_H_

#include "agent_control/server.h"

#include <string>

namespace agent_control
{

// Facts about the host application that app.info reports and that neither the
// server nor the locator can work out for itself.
struct AppInfo
{
    std::string app;          // "dashboard" or "editor"
    std::string config_path;  // May be empty (the editor can start with no config).
};

// Registers the methods that mean the same thing in every application:
// ui.snapshot, ui.find, ui.screenshot, input.click, input.key, input.type,
// app.info and app.quit.
//
// Application-specific verbs (editor.*, dashboard.*) are registered by the app
// itself via AgentServer::registerMethod.
void registerCoreMethods(AgentServer& server, AppInfo info);

}  // namespace agent_control

#endif  // AGENT_CONTROL_METHODS_H_
