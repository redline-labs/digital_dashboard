#ifndef AGENT_CONTROL_GUI_THREAD_H_
#define AGENT_CONTROL_GUI_THREAD_H_

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace agent_control
{

// Runs `fn` on the thread that owns `ctx` (in practice the Qt GUI thread) and
// waits up to `timeout_ms` for the result.
//
// WHY NOT Qt::BlockingQueuedConnection, which is the obvious answer: it waits
// forever. If the GUI thread is wedged -- an infinite paint loop, a deadlocked
// widget, a modal dialog nobody can close -- a blocking connection hangs the
// caller too, so the control socket dies at exactly the moment it was built to
// diagnose. A timeout turns "the app is stuck" from a hung client into a
// reportable answer.
//
// The promise is held by shared_ptr on purpose. After a timeout we return and
// the caller moves on, but the queued lambda is still sitting in Qt's event
// queue and may run much later; it then writes into a promise that is still
// alive rather than into a destroyed stack object.
//
// Returns nullopt if the deadline passed. Exceptions thrown by `fn` propagate to
// the caller (the dispatcher turns them into an INTERNAL error).
template <typename F>
auto callOnGuiThread(QObject* ctx, F&& fn, int timeout_ms) -> std::optional<std::invoke_result_t<F>>
{
    using R = std::invoke_result_t<F>;

    // Already on the target thread: posting to our own event queue and then
    // blocking on it would be a self-deadlock. Tests and in-process callers hit
    // this path.
    if (ctx == nullptr || QThread::currentThread() == ctx->thread())
    {
        return std::forward<F>(fn)();
    }

    auto promise = std::make_shared<std::promise<R>>();
    std::future<R> future = promise->get_future();

    QMetaObject::invokeMethod(
        ctx,
        [promise, task = std::function<R()>(std::forward<F>(fn))]() mutable
        {
            try
            {
                promise->set_value(task());
            }
            catch (...)
            {
                // set_exception itself throws if the promise is already
                // satisfied, which cannot happen here but must not escape into
                // Qt's event loop regardless.
                try
                {
                    promise->set_exception(std::current_exception());
                }
                catch (...)
                {
                }
            }
        },
        Qt::QueuedConnection);

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
    {
        return std::nullopt;
    }

    return future.get();
}

// Drains queued work so that a mutating call has actually taken effect by the
// time it returns. Without this, "click then screenshot" is a race: the click is
// delivered, but the repaint it triggered is still queued, and the screenshot
// captures the previous frame.
//
// Must be called from the GUI thread. Bounded by `budget_ms` so a widget that
// repaints continuously cannot pin us here forever.
void settleEventLoop(int budget_ms = 250);

}  // namespace agent_control

#endif  // AGENT_CONTROL_GUI_THREAD_H_
