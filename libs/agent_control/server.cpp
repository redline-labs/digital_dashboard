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

AgentServer::AgentServer(std::string app_name, QObject* parent) :
    QObject(parent),
    dispatcher_(std::move(app_name),
                [this](const std::string& method, const Dispatcher::Entry& entry, const json& params,
                       int timeout_ms) { return invoke(method, entry, params, timeout_ms); })
{
    socket_ = std::make_unique<ControlSocket>(
        [this](std::string_view line) { return handleLine(line); });
}

AgentServer::~AgentServer()
{
    stop();
}

void AgentServer::registerMethod(std::string name, Method handler, MethodKind kind)
{
    dispatcher_.registerMethod(std::move(name), std::move(handler), kind);
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

std::string AgentServer::handleLine(std::string_view line)
{
    return dispatcher_.handleLine(line);
}

MethodResult AgentServer::invoke(const std::string& method, const Dispatcher::Entry& entry,
                                 const json& params, int timeout_ms)
{
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

            MethodResult result = Dispatcher::runHandler(entry, params);
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
