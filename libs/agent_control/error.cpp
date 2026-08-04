#include "agent_control/error.h"

namespace agent_control
{

std::string_view toString(ErrorCode code)
{
    switch (code)
    {
        case ErrorCode::kBadParams:            return "BAD_PARAMS";
        case ErrorCode::kNoSuchMethod:         return "NO_SUCH_METHOD";
        case ErrorCode::kNoSuchWidget:         return "NO_SUCH_WIDGET";
        case ErrorCode::kAmbiguousSelector:    return "AMBIGUOUS_SELECTOR";
        case ErrorCode::kStaleRef:             return "STALE_REF";
        case ErrorCode::kWidgetNotVisible:     return "WIDGET_NOT_VISIBLE";
        case ErrorCode::kWidgetNotHitTestable: return "WIDGET_NOT_HIT_TESTABLE";
        case ErrorCode::kGuiThreadBusy:        return "GUI_THREAD_BUSY";
        case ErrorCode::kTimeout:              return "TIMEOUT";
        case ErrorCode::kUnsupportedInApp:     return "UNSUPPORTED_IN_APP";
        case ErrorCode::kInternal:             return "INTERNAL";
    }
    return "INTERNAL";
}

int jsonRpcCode(ErrorCode code)
{
    switch (code)
    {
        // The two that map onto the JSON-RPC 2.0 reserved range.
        case ErrorCode::kBadParams:            return -32602;  // Invalid params
        case ErrorCode::kNoSuchMethod:         return -32601;  // Method not found

        // Application-defined, outside the reserved range.
        case ErrorCode::kNoSuchWidget:         return 1001;
        case ErrorCode::kAmbiguousSelector:    return 1002;
        case ErrorCode::kStaleRef:             return 1003;
        case ErrorCode::kWidgetNotVisible:     return 1004;
        case ErrorCode::kWidgetNotHitTestable: return 1005;
        case ErrorCode::kGuiThreadBusy:        return 1006;
        case ErrorCode::kTimeout:              return 1007;
        case ErrorCode::kUnsupportedInApp:     return 1008;
        case ErrorCode::kInternal:             return 1009;
    }
    return 1009;
}

json AgentError::toJson() const
{
    json out;
    out["code"] = jsonRpcCode(code);
    out["message"] = message;

    json payload = data;
    payload["reason"] = std::string(toString(code));
    out["data"] = std::move(payload);
    return out;
}

AgentError badParams(std::string message)
{
    return AgentError{ErrorCode::kBadParams, std::move(message), json::object()};
}

AgentError noSuchWidget(std::string selector, json candidates)
{
    json data = json::object();
    data["selector"] = selector;
    data["candidates"] = std::move(candidates);
    return AgentError{ErrorCode::kNoSuchWidget,
                      "No widget matched selector '" + selector + "'.",
                      std::move(data)};
}

AgentError ambiguousSelector(std::string selector, json matches)
{
    const auto count = matches.is_array() ? matches.size() : 0u;
    json data = json::object();
    data["selector"] = selector;
    data["matches"] = std::move(matches);
    return AgentError{ErrorCode::kAmbiguousSelector,
                      "Selector '" + selector + "' matched " + std::to_string(count) +
                          " widgets; disambiguate with an index or a full path.",
                      std::move(data)};
}

AgentError internalError(std::string message)
{
    return AgentError{ErrorCode::kInternal, std::move(message), json::object()};
}

}  // namespace agent_control
