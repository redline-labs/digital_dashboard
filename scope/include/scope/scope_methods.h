#ifndef SCOPE_SCOPE_METHODS_H_
#define SCOPE_SCOPE_METHODS_H_

namespace agent_control
{
class AgentServer;
}

namespace scope
{

class ScopeWindow;

// Registers the scope.* verbs on `server`.
//
// Handlers run on the GUI thread and may touch widgets directly. Anything that
// changes state is registered kMutating, so the dispatcher drains the event
// loop before returning and a following screenshot observes the effect rather
// than the previous frame.
void registerScopeMethods(agent_control::AgentServer& server, ScopeWindow& window);

}  // namespace scope

#endif  // SCOPE_SCOPE_METHODS_H_
