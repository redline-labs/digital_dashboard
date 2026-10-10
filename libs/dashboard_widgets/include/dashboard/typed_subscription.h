#ifndef DASHBOARD_TYPED_SUBSCRIPTION_H_
#define DASHBOARD_TYPED_SUBSCRIPTION_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>

#include "dashboard/delivery_ticker.h"
#include "dashboard/staleness.h"
#include "pub_sub/raw_subscriber.h"
#include "pub_sub/typed_decode.h"

namespace dashboard
{

// The whole-message counterpart of ExpressionSubscription: subscribes to a key
// of a schema known at compile time, turns each message into a Value on the
// zenoh thread, and hands the latest one to the widget on the GUI thread
// through the shared DeliveryTicker -- coalesced, with the same staleness
// tracking.
//
// For a stream whose every message is a full snapshot (now playing, a map
// horizon, a session state), where a reading overwritten before the next frame
// is correctly never shown. NOT for event or media streams -- a page trigger's
// edges, H.264, PCM -- where dropping one corrupts what follows.
//
// `extract` runs on the zenoh RX thread with a Reader valid only for the call:
// it copies out what the widget needs, and is the place for work that must not
// block the GUI (decoding an image). It may keep state of its own between calls,
// which is how album art is decoded once per track rather than per message.
// Returning nullopt drops the message.
//
// Every sample passes pub_sub::SampleGate first -- schema name and revision --
// and a malformed one is dropped rather than taking the widget down.
//
// Not copyable or movable: the subscriber's callback captures `this`.
template <typename SchemaT, typename Value>
class TypedSubscription final : public DeliveryTarget
{
  public:
    using Reader = typename SchemaT::Reader;

    TypedSubscription(const std::string& zenoh_key,
                      std::function<std::optional<Value>(Reader)> extract,
                      std::function<void(Value)> deliver,
                      std::chrono::milliseconds stale_after,
                      std::function<void()> on_stale_edge)
        : extract_{std::move(extract)}
        , deliver_{std::move(deliver)}
        , on_stale_edge_{std::move(on_stale_edge)}
        , staleness_{staleness::suppressed() ? std::chrono::milliseconds{0} : stale_after,
                     std::chrono::steady_clock::now()}
        , ticker_{DeliveryTicker::instance()}
        , gate_{zenoh_key}
    {
        DeliveryTicker* const ticker = ticker_.data();
        subscriber_ = std::make_unique<pub_sub::RawSubscriber>(
            zenoh_key,
            [this, ticker](const std::vector<std::uint8_t>& bytes,
                           const pub_sub::RawSubscriber::SampleInfo& info)
            {
                // ON A ZENOH RX THREAD.
                std::optional<Value> value = decode(bytes, info);
                if (!value)
                {
                    return;
                }
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    pending_ = std::move(value);
                }
                ticker->wake();
            });
        ticker_->add(this);
    }

    ~TypedSubscription() override
    {
        // The subscriber first: undeclaring joins any in-flight callback, so
        // nothing below can still be written to once it is gone.
        subscriber_.reset();
        if (ticker_)
        {
            ticker_->remove(this);
        }
    }

    TypedSubscription(const TypedSubscription&) = delete;
    TypedSubscription& operator=(const TypedSubscription&) = delete;
    TypedSubscription(TypedSubscription&&) = delete;
    TypedSubscription& operator=(TypedSubscription&&) = delete;

    bool isValid() const { return subscriber_ && subscriber_->isValid(); }

    // True once nothing has arrived for the timeout; never with a timeout of 0.
    bool isStale() const { return staleness_.isStale(); }

    void drain(clock::time_point now) override
    {
        std::optional<Value> value;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            value.swap(pending_);
        }

        const bool became_fresh =
            value && staleness_.onSample(now) == StalenessTracker::Edge::became_fresh;
        if (value)
        {
            deliver_(std::move(*value));
        }
        const bool became_stale = staleness_.poll(now) == StalenessTracker::Edge::became_stale;

        if ((became_fresh || became_stale) && on_stale_edge_)
        {
            on_stale_edge_();
        }
    }

    std::optional<clock::time_point> staleDeadline() const override { return staleness_.deadline(); }

  private:
    std::optional<Value> decode(const std::vector<std::uint8_t>& bytes,
                                const pub_sub::RawSubscriber::SampleInfo& info)
    {
        std::optional<Value> value;
        if (gate_.admit(info.schema_name, info.layout))
        {
            pub_sub::decodeAs<SchemaT>(gate_, bytes, [&](Reader reader) { value = extract_(reader); });
        }
        return value;
    }

    std::function<std::optional<Value>(Reader)> extract_;
    std::function<void(Value)> deliver_;
    std::function<void()> on_stale_edge_;
    StalenessTracker staleness_;
    QPointer<DeliveryTicker> ticker_;
    pub_sub::TypedGate<SchemaT> gate_;

    std::mutex mutex_;
    std::optional<Value> pending_;

    std::unique_ptr<pub_sub::RawSubscriber> subscriber_;
};

template <typename SchemaT, typename Value>
using TypedSubscriptionPtr = std::unique_ptr<TypedSubscription<SchemaT, Value>>;

// Builds one for `receiver`, delivering to `setter` on the GUI thread and
// repainting on a staleness edge. An empty key returns nullptr and logs nothing:
// an unbound input, not a fault.
template <typename SchemaT, typename Value, typename Receiver, typename Setter>
TypedSubscriptionPtr<SchemaT, Value> makeTypedSubscription(
    const std::string& zenoh_key, std::function<std::optional<Value>(typename SchemaT::Reader)> extract,
    Receiver* receiver, Setter setter, std::chrono::milliseconds stale_after = std::chrono::milliseconds{0})
{
    if (zenoh_key.empty())
    {
        return nullptr;
    }
    return std::make_unique<TypedSubscription<SchemaT, Value>>(
        zenoh_key, std::move(extract),
        [receiver, setter](Value value) { std::invoke(setter, receiver, std::move(value)); },
        stale_after, [receiver]() { receiver->update(); });
}

// Null-safe: an unbound subscription is never stale.
template <typename SchemaT, typename Value>
bool isStale(const TypedSubscriptionPtr<SchemaT, Value>& subscription)
{
    return subscription && subscription->isStale();
}

}  // namespace dashboard

#endif  // DASHBOARD_TYPED_SUBSCRIPTION_H_
