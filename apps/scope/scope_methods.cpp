#include "scope/scope_methods.h"

#include "scope_methods_detail.h"

namespace scope
{

void registerScopeMethods(agent_control::AgentServer& server, ScopeWindow& window)
{
    // One registrar so every verb gets the flushSeek() wrapper; one call per
    // area, each in its own file -- see scope_methods_detail.h for the map.
    const methods_detail::FlushedRegistrar registerFlushed(server, window);

    methods_detail::registerPanelMethods(registerFlushed, window);
    methods_detail::registerTimeMethods(registerFlushed, window);
    methods_detail::registerSourceMethods(registerFlushed, window);
    methods_detail::registerConfigMethods(registerFlushed, window);
}

}  // namespace scope
