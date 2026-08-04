#include "agent_control/gui_thread.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>

namespace agent_control
{

void settleEventLoop(int budget_ms)
{
    QElapsedTimer timer;
    timer.start();

    // "Did anything happen?" is the signal we want, so that a settled UI costs
    // one pass rather than the whole budget. Qt6 removed hasPendingEvents() and
    // QCoreApplication::processEvents() returns void, but the thread's event
    // dispatcher still reports whether it processed anything -- so go through it
    // directly. A widget that repaints continuously (the CarPlay video path
    // does, at 30 Hz) never drains, and the elapsed-time budget is what
    // terminates the loop there.
    QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance();
    if (dispatcher == nullptr)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, budget_ms);
    }
    else
    {
        while (timer.elapsed() < budget_ms)
        {
            if (!dispatcher->processEvents(QEventLoop::AllEvents))
            {
                break;
            }
        }
    }

    // deleteLater() work is not delivered by processEvents() when called from
    // inside an event handler, and widget teardown is exactly what a mutating
    // method tends to queue (set_config rebuilds a widget). Flush it explicitly
    // so the next snapshot does not report widgets that are already dead.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}  // namespace agent_control
