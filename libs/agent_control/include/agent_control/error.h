#ifndef AGENT_CONTROL_ERROR_H_
#define AGENT_CONTROL_ERROR_H_

#include <nlohmann/json.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace agent_control
{

using json = nlohmann::json;

// Stable, machine-readable failure reasons.
//
// Every one of these is something a caller can act on differently, which is the
// bar for earning a code: an ambiguous selector means "say which one you meant",
// a stale ref means "re-snapshot", a busy GUI thread means "the app is wedged,
// go read the logs". A bare message string would collapse all three into
// "something went wrong", which is what makes remote-control interfaces
// untrustworthy to drive.
enum class ErrorCode
{
    kBadParams,             // Missing/ill-typed parameters.
    kNoSuchMethod,          // Unknown method name.
    kNoSuchWidget,          // Selector matched nothing.
    kAmbiguousSelector,     // Selector matched more than one widget.
    kStaleRef,              // Ref from a superseded snapshot revision.
    kWidgetNotVisible,      // Target exists but cannot be seen/interacted with.
    kWidgetNotHitTestable,  // Target is transparent to mouse events.
    kGuiThreadBusy,         // GUI thread did not run our call in time.
    kTimeout,               // Operation itself timed out.
    kUnsupportedInApp,      // Method exists but not in this application.
    kInternal,              // Handler threw, or an invariant broke.
};

std::string_view toString(ErrorCode code);

// JSON-RPC error codes. The spec reserves -32768..-32000; everything
// application-defined lives outside that range, so ours start at -32050 only
// for the two that genuinely map onto spec errors and use 1000+ otherwise.
int jsonRpcCode(ErrorCode code);

struct AgentError
{
    ErrorCode code = ErrorCode::kInternal;
    std::string message;

    // Extra structured context. Populated for the codes where the caller needs
    // more than prose to recover -- candidate widget paths for a failed or
    // ambiguous selector, the current revision for a stale ref.
    json data = json::object();

    json toJson() const;
};

// Convenience builders so handlers read as `return badParams("...")`.
AgentError badParams(std::string message);
AgentError noSuchWidget(std::string selector, json candidates);
AgentError ambiguousSelector(std::string selector, json matches);
AgentError internalError(std::string message);

template <typename T>
using Result = std::expected<T, AgentError>;

using MethodResult = Result<json>;

}  // namespace agent_control

#endif  // AGENT_CONTROL_ERROR_H_
