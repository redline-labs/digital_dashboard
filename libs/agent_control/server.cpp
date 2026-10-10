#include "agent_control/server.h"

#include "agent_control/control_socket.h"
#include "agent_control/gui_thread.h"
#include "agent_control/inspector.h"

#include <QApplication>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
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
    //
    // Not callOnGuiThread(): a handler can open a modal -- a dialog's exec(), a
    // menu's exec() -- and then not return until someone closes it. The call is
    // answered as soon as one opens, describing it, and the handler stays parked
    // in the modal's own event loop, which keeps running every later call. So
    // the dialog is driven like any other widget, and its handler finishes when
    // it closes.
    //
    // Everything the queued task uses is held by value or shared_ptr: after an
    // early answer or a timeout this function returns while the task is still
    // queued or still parked, and a reference into this frame would dangle.
    auto call = std::make_shared<PendingCall>();
    std::future<MethodResult> future = call->promise.get_future();

    auto task = [this, call, entry, params, method]()
        {
            // Widgets only exist under a QApplication; the dispatcher also runs
            // under a bare QCoreApplication, where asking would crash.
            const bool widgets = qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr;
            QWidget* const modal_before = widgets ? QApplication::activeModalWidget() : nullptr;
            QWidget* const popup_before = widgets ? QApplication::activePopupWidget() : nullptr;

            // Fires only inside a nested event loop, which is exactly when a
            // handler that has not returned has opened something modal.
            QTimer watch;
            watch.setInterval(kModalPollMs);
            QObject::connect(&watch, &QTimer::timeout, &watch,
                             [this, &watch, call, modal_before, popup_before, &method]()
                             {
                                 QWidget* opened = QApplication::activePopupWidget();
                                 if (opened == popup_before)
                                 {
                                     opened = QApplication::activeModalWidget();
                                     if (opened == modal_before)
                                     {
                                         opened = nullptr;
                                     }
                                 }
                                 if (opened != nullptr)
                                 {
                                     watch.stop();
                                     call->answer(modalOpened(method, opened));
                                 }
                             });
            if (widgets)
            {
                watch.start();
            }

            MethodResult result = [&]() -> MethodResult
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
            }();
            watch.stop();

            if (call->answered())
            {
                SPDLOG_INFO("[agent] '{}' finished after the modal it opened closed", method);
                return;
            }
            if (result.has_value() && entry.kind == MethodKind::kMutating)
            {
                settleEventLoop();
            }
            call->answer(std::move(result));
        };

    // Already on the GUI thread (tests, in-process callers): posting to our own
    // queue and blocking on it would deadlock.
    if (QThread::currentThread() == thread())
    {
        task();
    }
    else
    {
        QMetaObject::invokeMethod(this, std::move(task), Qt::QueuedConnection);
    }

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
    {
        SPDLOG_WARN("[agent] '{}' timed out after {} ms waiting for the GUI thread",
                    method, timeout_ms);
        json data = json::object();
        data["method"] = method;
        data["timeout_ms"] = timeout_ms;
        return std::unexpected(AgentError{
            ErrorCode::kGuiThreadBusy,
            "The GUI thread did not run '" + method + "' within " + std::to_string(timeout_ms) +
                " ms. It is blocked or saturated with repaints.",
            std::move(data)});
    }

    return future.get();
}

json AgentServer::modalOpened(const std::string& method, QWidget* modal)
{
    json out = json::object();
    out["modal_opened"] = true;
    out["modal"] = describeWidget(locator(), modal);
    out["note"] = "'" + method +
                  "' opened this and is waiting for it to close; its own result is not "
                  "reported. Drive it with ui.* and input.* like any other widget -- a "
                  "button click, input.key Escape, or a menu item -- and the app carries on "
                  "from there.";
    return out;
}

}  // namespace agent_control
