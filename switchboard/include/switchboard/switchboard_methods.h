#ifndef SWITCHBOARD_SWITCHBOARD_METHODS_H_
#define SWITCHBOARD_SWITCHBOARD_METHODS_H_

#include "agent_control/server.h"

namespace switchboard
{

class SwitchboardWindow;

// The switchboard.* agent verbs. They drive the WINDOW -- select a service, fill
// the form, press Submit -- rather than calling services directly, so an agent
// exercising them is exercising what a person would use.
//
// A call is two steps because handlers run on the GUI thread and a reply can
// take the whole timeout: switchboard.submit returns a call id at once, and
// switchboard.result is polled until the call is no longer pending.
void registerSwitchboardMethods(agent_control::AgentServer& server, SwitchboardWindow& window);

}  // namespace switchboard

#endif  // SWITCHBOARD_SWITCHBOARD_METHODS_H_
