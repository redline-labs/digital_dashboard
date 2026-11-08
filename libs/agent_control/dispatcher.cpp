#include "agent_control/dispatcher.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace agent_control
{

namespace
{

// A JSON-RPC error response for failures that happen before (or instead of) a
// method call: unparseable input, a malformed envelope, an unknown method.
json errorResponse(const json& id, const AgentError& error)
{
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error.toJson();
    return response;
}

json resultResponse(const json& id, json result)
{
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

}  // namespace

Dispatcher::Dispatcher(std::string app_name, Invoker invoker) :
    app_name_(std::move(app_name)), invoker_(std::move(invoker))
{
    // Discovery, so a client can check what this build actually supports rather
    // than guessing from its own version.
    registerMethod("rpc.methods",
                   [this](const json&) -> MethodResult
                   {
                       json out = json::object();
                       out["app"] = app_name_;
                       out["methods"] = methodNames();
                       json summaries = json::object();
                       {
                           std::lock_guard<std::mutex> lock(mutex_);
                           for (const auto& [name, entry] : methods_)
                           {
                               if (!entry.summary.empty())
                               {
                                   summaries[name] = entry.summary;
                               }
                           }
                       }
                       if (!summaries.empty())
                       {
                           out["summaries"] = std::move(summaries);
                       }
                       return out;
                   });
}

void Dispatcher::registerMethod(std::string name, Method handler, MethodKind kind, std::string summary)
{
    std::lock_guard<std::mutex> lock(mutex_);
    methods_[std::move(name)] = Entry{std::move(handler), kind, std::move(summary)};
}

std::vector<std::string> Dispatcher::methodNames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(methods_.size());
    for (const auto& [name, entry] : methods_)
    {
        names.push_back(name);
    }
    return names;
}

MethodResult Dispatcher::runHandler(const Entry& entry, const json& params)
{
    try
    {
        return entry.handler(params);
    }
    catch (const std::exception& e)
    {
        return std::unexpected(internalError(std::string("Handler threw: ") + e.what()));
    }
    catch (...)
    {
        return std::unexpected(internalError("Handler threw a non-std exception."));
    }
}

std::string Dispatcher::handleLine(std::string_view line)
{
    json request;
    try
    {
        request = json::parse(line);
    }
    catch (const json::exception& e)
    {
        SPDLOG_WARN("[agent] unparseable request: {}", e.what());
        return errorResponse(nullptr,
                             AgentError{ErrorCode::kBadParams,
                                        std::string("Request is not valid JSON: ") + e.what(),
                                        json::object()})
            .dump();
    }

    if (!request.is_object())
    {
        return errorResponse(nullptr, badParams("Request must be a JSON object.")).dump();
    }

    return dispatch(request).dump();
}

json Dispatcher::dispatch(const json& request)
{
    // id is echoed even when the rest of the envelope is wrong, so a client can
    // always correlate a failure with the call that caused it.
    const json id = request.contains("id") ? request["id"] : json(nullptr);

    if (!request.contains("method") || !request["method"].is_string())
    {
        return errorResponse(id, badParams("Request is missing a string 'method'."));
    }
    const auto method = request["method"].get<std::string>();

    json params = json::object();
    if (request.contains("params"))
    {
        if (!request["params"].is_object())
        {
            return errorResponse(id, badParams("'params' must be an object."));
        }
        params = request["params"];
    }

    int timeout_ms = kDefaultTimeoutMs;
    if (params.contains("_timeout_ms"))
    {
        if (!params["_timeout_ms"].is_number_integer())
        {
            return errorResponse(id, badParams("'_timeout_ms' must be an integer."));
        }
        timeout_ms = params["_timeout_ms"].get<int>();
        if (timeout_ms <= 0)
        {
            return errorResponse(id, badParams("'_timeout_ms' must be positive."));
        }
    }

    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = methods_.find(method);
        if (it == methods_.end())
        {
            return errorResponse(id, AgentError{ErrorCode::kNoSuchMethod,
                                                "No such method '" + method + "'.", json::object()});
        }
        entry = it->second;
    }

    MethodResult result = invoker_ ? invoker_(method, entry, params, timeout_ms) : runHandler(entry, params);
    if (!result.has_value())
    {
        return errorResponse(id, result.error());
    }
    return resultResponse(id, std::move(result.value()));
}

}  // namespace agent_control
