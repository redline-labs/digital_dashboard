#include "agent_control/server.h"

#include "agent_control/control_socket.h"
#include "agent_control/gui_thread.h"

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

AgentServer::AgentServer(std::string app_name, QObject* parent) :
    QObject(parent), app_name_(std::move(app_name))
{
    socket_ = std::make_unique<ControlSocket>(
        [this](std::string_view line) { return handleLine(line); });
    registerBuiltins();
}

AgentServer::~AgentServer()
{
    stop();
}

void AgentServer::registerMethod(std::string name, Method handler, MethodKind kind)
{
    std::lock_guard<std::mutex> lock(methods_mutex_);
    methods_[std::move(name)] = Entry{std::move(handler), kind};
}

bool AgentServer::start(const std::string& socket_path)
{
    return socket_->start(socket_path);
}

void AgentServer::stop()
{
    if (socket_)
    {
        socket_->stop();
    }
}

std::string AgentServer::socketPath() const
{
    return socket_ ? socket_->socketPath() : std::string{};
}

std::vector<std::string> AgentServer::methodNames() const
{
    std::lock_guard<std::mutex> lock(methods_mutex_);
    std::vector<std::string> names;
    names.reserve(methods_.size());
    for (const auto& [name, entry] : methods_)
    {
        names.push_back(name);
    }
    return names;
}

void AgentServer::registerBuiltins()
{
    // Discovery, so a client can check what this build actually supports rather
    // than guessing from its own version. The escape-hatch tool on the Python
    // side reads this.
    registerMethod("rpc.methods",
                   [this](const json&) -> MethodResult
                   {
                       json out = json::object();
                       out["app"] = app_name_;
                       out["methods"] = methodNames();
                       return out;
                   });
}

std::string AgentServer::handleLine(std::string_view line)
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

json AgentServer::dispatch(const json& request)
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

    MethodResult result = invoke(method, params, timeout_ms);
    if (!result.has_value())
    {
        return errorResponse(id, result.error());
    }
    return resultResponse(id, std::move(result.value()));
}

MethodResult AgentServer::invoke(const std::string& method, const json& params, int timeout_ms)
{
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(methods_mutex_);
        const auto it = methods_.find(method);
        if (it == methods_.end())
        {
            return std::unexpected(AgentError{ErrorCode::kNoSuchMethod,
                                              "No such method '" + method + "'.",
                                              json::object()});
        }
        entry = it->second;
    }

    // Hop to the GUI thread. Everything a handler touches -- the widget tree,
    // the backing store, event delivery -- is thread-affine to it, so this is
    // the only place that hop needs to exist.
    auto outcome = callOnGuiThread(
        this,
        [&entry, &params]() -> MethodResult
        {
            try
            {
                MethodResult r = entry.handler(params);
                if (r.has_value() && entry.kind == MethodKind::kMutating)
                {
                    settleEventLoop();
                }
                return r;
            }
            catch (const std::exception& e)
            {
                return std::unexpected(
                    internalError(std::string("Handler threw: ") + e.what()));
            }
            catch (...)
            {
                return std::unexpected(internalError("Handler threw a non-std exception."));
            }
        },
        timeout_ms);

    if (!outcome.has_value())
    {
        SPDLOG_WARN("[agent] '{}' timed out after {} ms waiting for the GUI thread",
                    method, timeout_ms);
        json data = json::object();
        data["method"] = method;
        data["timeout_ms"] = timeout_ms;
        return std::unexpected(AgentError{
            ErrorCode::kGuiThreadBusy,
            "The GUI thread did not run '" + method + "' within " + std::to_string(timeout_ms) +
                " ms. It is blocked, in a modal loop, or saturated with repaints.",
            std::move(data)});
    }

    return std::move(outcome.value());
}

}  // namespace agent_control
