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

#include "pub_sub/zenoh_subscriber.h"
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
                           std::chrono::milliseconds interval = kDeliveryInterval)
        : deliver_{std::move(deliver)}
    {
        subscriber_ = std::make_unique<pub_sub::ZenohExpressionSubscriber>(schema_type, expression, zenoh_key);
        if (!subscriber_->isValid())
        {
            return;
        }

        // Runs on the zenoh RX thread. It takes a short mutex and nothing else:
        // no allocation, no Qt call, no event posted.
        subscriber_->setResultCallback<T>([this](T value)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_ = value;
            last_sample_ = std::chrono::steady_clock::now();
        });

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

    // How long since this subscription last produced a usable value, or nullopt
    // if it never has.
    //
    // Nothing in the dashboard notices when a publisher stops: every gauge holds
    // its last reading indefinitely, so a dead sensor and a steady one look
    // identical. That is the wrong failure for a vehicle display, and it gets
    // more likely with every stream added.
    //
    // This is the measurement, deliberately without a policy attached. What
    // counts as "too long" is per-stream -- an odometer that publishes on change
    // and a 100 Hz wheel speed cannot share a threshold -- and how a stale gauge
    // should *look* is a design decision per widget. Both belong with whoever is
    // adding the streams, not baked in here.
    std::optional<std::chrono::steady_clock::duration> sinceLastSample() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!last_sample_)
        {
            return std::nullopt;
        }
        return std::chrono::steady_clock::now() - *last_sample_;
    }

  private:
    void drain()
    {
        std::optional<T> value;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            value.swap(pending_);
        }

        // Nothing arrived since the last tick: an idle subscription costs a
        // mutex acquire and no repaint.
        if (value)
        {
            deliver_(*value);
        }
    }

    mutable std::mutex mutex_;
    std::optional<T> pending_;
    std::optional<std::chrono::steady_clock::time_point> last_sample_;
    std::function<void(T)> deliver_;
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
// Returns nullptr, with the error logged under `log_context`, if construction
// or validation fails. `setter` is a member-function pointer of Receiver (or any
// callable invocable as setter(receiver, value)).
template <typename T, typename Receiver, typename Setter>
ExpressionSubscriptionPtr<T> makeExpressionSubscription(
    pub_sub::schema_type_t schema_type,
    const std::string& expression,
    const std::string& zenoh_key,
    Receiver* receiver,
    Setter setter,
    const char* log_context)
{
    ExpressionSubscriptionPtr<T> subscription;
    try
    {
        subscription = std::make_unique<ExpressionSubscription<T>>(
            schema_type, expression, zenoh_key,
            [receiver, setter](T value) { std::invoke(setter, receiver, value); });
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}: failed to initialize expression subscriber: {}", log_context, e.what());
        return nullptr;
    }

    if (!subscription->isValid())
    {
        SPDLOG_ERROR("{}: invalid expression '{}' for schema '{}'", log_context, expression,
                     reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type));
        return nullptr;
    }

    return subscription;
}

}  // namespace dashboard

#endif  // DASHBOARD_EXPRESSION_SUBSCRIPTION_H_
