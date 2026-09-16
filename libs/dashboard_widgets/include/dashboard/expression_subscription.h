#ifndef DASHBOARD_EXPRESSION_SUBSCRIPTION_H_
#define DASHBOARD_EXPRESSION_SUBSCRIPTION_H_

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <QObject>
#include <QTimer>

#include <spdlog/spdlog.h>

// The lean header, not pub_sub/zenoh_subscriber.h: this is what every widget
// reaches the bus through, so what it drags in is paid for across the whole
// dashboard. ZenohTypedSubscriber lives in the other one and needs capnp in its
// header; nothing here does.
#include "dashboard/staleness.h"
#include "pub_sub/expression_subscriber.h"
#include "reflection/reflection.h"

namespace dashboard {

// How often a subscription hands its latest value to the GUI. A gauge cannot
// show more than the display refresh, so ~60 Hz is the most that can possibly
// be useful; anything faster is work nobody sees.
inline constexpr std::chrono::milliseconds kDeliveryInterval{16};

// A zenoh expression subscription whose samples are *coalesced* before they
// reach the widget.
//
// The zenoh thread writes each evaluated value into a one-slot mailbox; a
// GUI-thread timer takes whatever is in the slot and delivers it. A value that
// is overwritten before the next tick is simply never shown, which is the
// correct thing to do with a stale reading.
//
// The old shape posted one QMetaCallEvent per sample, per subscription, with no
// bound and no coalescing. That is fine while the GUI keeps up and unbounded
// when it does not: Qt's posted-event queue grows without limit, and because
// nothing discards stale entries the display then sweeps through a backlog
// instead of jumping to the current value -- it looks smooth while falling
// further behind. On an embedded target that is also unbounded memory growth.
// Bounding the queue at one entry per subscription makes the failure mode
// "drops old readings", which is what a gauge wants.
//
// Not copyable or movable: the zenoh callback captures `this`, so the address
// has to stay put. Construct it through makeExpressionSubscription().
template <typename T>
class ExpressionSubscription
{
  public:
    ExpressionSubscription(pub_sub::schema_type_t schema_type,
                           const std::string& expression,
                           const std::string& zenoh_key,
                           std::function<void(T)> deliver,
                           std::chrono::milliseconds stale_after,
                           std::function<void()> on_stale_edge,
                           std::chrono::milliseconds interval = kDeliveryInterval)
        : deliver_{std::move(deliver)}
        , on_stale_edge_{std::move(on_stale_edge)}
        , staleness_{staleness::suppressed() ? std::chrono::milliseconds{0} : stale_after,
                     std::chrono::steady_clock::now()}
    {
        subscriber_ = std::make_unique<pub_sub::ZenohExpressionSubscriber>(schema_type, expression, zenoh_key);
        if (subscriber_->isValid())
        {
            // Runs on the zenoh RX thread. It takes a short mutex and nothing
            // else: no allocation, no Qt call, no event posted.
            subscriber_->setResultCallback<T>([this](T value)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                pending_ = value;
            });
        }

        // Started even when the expression did not compile, so that a binding
        // which cannot ever deliver goes stale like one that has stopped. A
        // gauge reading zero because its expression is broken is worse than one
        // saying it has no data.
        //
        // The timer is a plain member, so it belongs to the thread that
        // constructed this -- the GUI thread -- and it is also the connection's
        // context object, so the connection dies with it.
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this]() { drain(); });
        timer_.start(interval);
    }

    ExpressionSubscription(const ExpressionSubscription&) = delete;
    ExpressionSubscription& operator=(const ExpressionSubscription&) = delete;
    ExpressionSubscription(ExpressionSubscription&&) = delete;
    ExpressionSubscription& operator=(ExpressionSubscription&&) = delete;

    bool isValid() const { return subscriber_ && subscriber_->isValid(); }

    // True once nothing has arrived for the binding's timeout. Always false for
    // a binding that did not ask for one, and in a process that suppressed
    // staleness.
    //
    // This is where a widget reads its no-data state: it holds the
    // subscription, so it asks the subscription. There is no second copy of the
    // flag on the widget to keep in step, and the edge hook exists only to
    // schedule the repaint that will come here and ask.
    bool isStale() const { return staleness_.isStale(); }

  private:
    void drain()
    {
        std::optional<T> value;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            value.swap(pending_);
        }

        // The staleness edges are detected here, on the GUI thread, so the
        // repaint is scheduled where the painting happens. A value that arrives
        // counts as fresh even if the widget then discards it: what is being
        // measured is the stream, not the reading.
        //
        // The hook says only THAT the answer changed, never what it is. The
        // widget reads isStale() when it next paints.
        const auto now = std::chrono::steady_clock::now();
        const bool became_fresh =
            value && staleness_.onSample(now) == StalenessTracker::Edge::became_fresh;

        // Nothing arrived since the last tick: an idle subscription costs a
        // mutex acquire and no repaint.
        if (value)
        {
            deliver_(*value);
        }

        const bool became_stale = staleness_.poll(now) == StalenessTracker::Edge::became_stale;

        if ((became_fresh || became_stale) && on_stale_edge_)
        {
            on_stale_edge_();
        }
    }

    mutable std::mutex mutex_;
    std::optional<T> pending_;
    std::function<void(T)> deliver_;
    std::function<void()> on_stale_edge_;
    StalenessTracker staleness_;
    QTimer timer_;

    // Declared last so it is destroyed FIRST. zenoh's undeclare joins in-flight
    // callbacks, so by the time the members above are destroyed no callback can
    // still be writing to them.
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> subscriber_;
};

template <typename T>
using ExpressionSubscriptionPtr = std::unique_ptr<ExpressionSubscription<T>>;

// Builds a coalescing subscription for `expression`, validates it, and delivers
// results of type T to `setter` on `receiver` -- always on the GUI thread.
// `setter` is a member-function pointer of Receiver (or any callable invocable
// as setter(receiver, value)).
//
// After `stale_after` with nothing arriving, isStale() turns true and the
// widget is repainted; both reverse when a reading returns. The widget asks
// isStale() where it paints -- that is the only copy of the answer, so there is
// no flag on the widget to keep in step with it. Zero means never stale, which
// is also what the editor forces process-wide.
//
// Returns nullptr only if construction threw. An expression that does not
// compile still yields a subscription, because that subscription is what
// reports no data: a gauge showing zero for a broken binding is worse than one
// showing nothing.
//
// Failures name the key. It is what a config author wrote, what `inspect echo`
// takes, and what tells two bindings of the same widget apart -- which a label
// passed in by the caller could only do by being kept in step with the code by
// hand.
template <typename T, typename Receiver, typename Setter>
ExpressionSubscriptionPtr<T> makeExpressionSubscription(
    pub_sub::schema_type_t schema_type,
    const std::string& expression,
    const std::string& zenoh_key,
    Receiver* receiver,
    Setter setter,
    std::chrono::milliseconds stale_after)
{
    ExpressionSubscriptionPtr<T> subscription;
    try
    {
        subscription = std::make_unique<ExpressionSubscription<T>>(
            schema_type, expression, zenoh_key,
            [receiver, setter](T value) { std::invoke(setter, receiver, value); }, stale_after,
            [receiver]() { receiver->update(); });
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("'{}': failed to initialize expression subscriber: {}", zenoh_key,
                     e.what());
        return nullptr;
    }

    // Returned even when the expression did not compile, unlike the overload
    // above: the subscription is what reports no data, so throwing it away
    // would leave the gauge showing zero rather than showing nothing. The error
    // is still said once, here.
    if (!subscription->isValid())
    {
        SPDLOG_ERROR("'{}': invalid expression '{}' for schema '{}'", zenoh_key, expression,
                     reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type));
    }

    return subscription;
}

}  // namespace dashboard

#endif  // DASHBOARD_EXPRESSION_SUBSCRIPTION_H_
