#ifndef DASHBOARD_PAGE_METHODS_H_
#define DASHBOARD_PAGE_METHODS_H_

#include "agent_control/server.h"

namespace dashboard::agent
{

// Registers pages.list and pages.command: every page_stack in the application's
// windows, what each shows, and a way to change it without the bus -- which is
// what makes a page switch testable when nothing is publishing.
void registerPageMethods(agent_control::AgentServer& server);

}  // namespace dashboard::agent

#endif  // DASHBOARD_PAGE_METHODS_H_
