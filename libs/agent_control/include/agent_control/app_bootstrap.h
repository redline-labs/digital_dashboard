#ifndef AGENT_CONTROL_APP_BOOTSTRAP_H_
#define AGENT_CONTROL_APP_BOOTSTRAP_H_

#include "agent_control/mcp_option.h"

#include <string>

namespace agent_control
{

class AgentServer;

// What every GUI app does to be driven by an agent, said once: the dashboard,
// the editor, scope and switchboard each carried their own copy of the --mcp
// option, the leftover-argument check, the headless setup and the readiness
// handshake. The parts without Qt are in mcp_option.h.

// Before QApplication is constructed -- the platform plugin is chosen then and
// cannot change afterwards: forces the offscreen platform (no display, no
// window stealing focus) and installs the log ring behind app.logs.
void prepareHeadless();

// Starts `server` on `socket_path` and prints the readiness line a supervisor
// waits for. False, logged, if the socket would not open.
//
// The line rather than polling for the socket file: the socket exists from the
// moment bind() returns, which is before the window is up, so a poller would
// connect too early and see an empty widget tree.
bool startAndAnnounce(AgentServer& server, const std::string& socket_path);

}  // namespace agent_control

#endif  // AGENT_CONTROL_APP_BOOTSTRAP_H_
