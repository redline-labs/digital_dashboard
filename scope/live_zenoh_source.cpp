#include "scope/live_zenoh_source.h"

#include "pub_sub/expression_evaluator.h"
#include "pub_sub/raw_subscriber.h"
#include "pub_sub/topic_directory.h"

#include <reflection/reflection.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace scope
{

namespace
{

// One signal bound to a key: the expression that turns a payload into a number,
// and where the number goes.
//
// Held by shared_ptr from both the key's binding list and, transitively, from
// whatever the sample callback loaded -- so a callback that is already running
// when a signal is released keeps everything it touches alive to the end.
struct Binding
{
    SignalHandle handle = kInvalidSignal;
    std::unique_ptr<pub_sub::ExpressionEvaluator> evaluator;
    std::shared_ptr<SignalBuffer> buffer;
};

using BindingList = std::vector<std::shared_ptr<Binding>>;

}  // namespace

// One zenoh subscription, plus everything bound to its key.
struct KeySubscription
{
    // Copy-on-write. The zenoh RX thread reads the list; the GUI thread
    // replaces it whole on bind and release.
    //
    // The mutex is held for exactly one shared_ptr copy on the read side and
    // one pointer assignment on the write side -- never across a decode. That
    // distinction is the whole design: iterating the bindings means capnp
    // decoding and exprtk evaluation for each one, and holding a lock across
    // that would let a busy bus stall the GUI thread on every bind, or a
    // stalled GUI thread hold up the bus. Copying the pointer out and walking
    // it unlocked costs an atomic increment.
    //
    // std::atomic<std::shared_ptr<>> would say this more directly, but libc++
    // has not implemented that specialization -- it static_asserts on
    // is_trivially_copyable. The free-function std::atomic_load/store overloads
    // are deprecated in C++20. So: a mutex, scoped tightly, which is what those
    // are usually implemented with anyway.
    mutable std::mutex bindings_mutex;
    std::shared_ptr<const BindingList> bindings;

    std::shared_ptr<const BindingList> snapshot() const
    {
        const std::lock_guard<std::mutex> guard(bindings_mutex);
        return bindings;
    }

    void publish(std::shared_ptr<const BindingList> updated)
    {
        const std::lock_guard<std::mutex> guard(bindings_mutex);
        bindings = std::move(updated);
    }

    // Declared last so it is destroyed FIRST. zenoh's undeclare joins in-flight
    // callbacks, so once this is gone nothing can still be reading `bindings`.
    // The reverse order is a use-after-free on every teardown; the same
    // ordering rule holds in ZenohExpressionSubscriber and RawSubscriber, and
    // it has bitten this tree before.
    std::unique_ptr<pub_sub::RawSubscriber> subscriber;
};

struct LiveZenohSource::Impl
{
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    // What is advertised on the bus. Owned by the source rather than by
    // whatever draws a picker: "which topics exist" is a property of where the
    // data comes from, and putting it here is what lets a recorded source
    // answer the same question from a file index with nothing above it
    // changing.
    pub_sub::TopicDirectory directory;

    // Key -> its one subscription. std::map rather than unordered_map because
    // node addresses have to stay put: the sample callback captures a pointer
    // to the KeySubscription, and rehashing would not move the node but a
    // different container might.
    std::map<std::string, std::unique_ptr<KeySubscription>> by_key;

    // Handle -> the key it was bound on, so release() knows where to look.
    std::map<SignalHandle, std::string> handle_keys;

    SignalHandle next_handle = 1;  // 0 is kInvalidSignal.

    double secondsSince(std::chrono::steady_clock::time_point when) const
    {
        return std::chrono::duration<double>(when - t0).count();
    }
};

LiveZenohSource::LiveZenohSource() : impl_(std::make_unique<Impl>())
{
}

LiveZenohSource::~LiveZenohSource() = default;

SourceCaps LiveZenohSource::caps() const
{
    SourceCaps caps;
    caps.live = true;
    caps.seekable = false;
    return caps;
}

std::vector<TopicInfo> LiveZenohSource::topics() const
{
    // Straight from the advertisements. No listening, no window, no blocking --
    // a topic is here because its publisher exists, not because it happened to
    // send something while someone was watching.
    std::vector<TopicInfo> out;
    for (const pub_sub::DirectoryEntry& entry : impl_->directory.snapshot())
    {
        TopicInfo info;
        info.key = entry.key;
        info.schema = entry.schema;
        info.reachable = entry.reachable;
        out.push_back(std::move(info));
    }
    return out;
}

std::uint64_t LiveZenohSource::topicsRevision() const
{
    return impl_->directory.revision();
}

SignalHandle LiveZenohSource::bind(const SignalKey& key, std::shared_ptr<SignalBuffer> into)
{
    if (key.zenoh_key.empty() || key.value_expression.empty() || !into)
    {
        SPDLOG_ERROR("Refusing to bind a signal with an empty key, expression or buffer.");
        return kInvalidSignal;
    }

    auto evaluator = std::make_unique<pub_sub::ExpressionEvaluator>(
        key.schema_type, key.value_expression, key.zenoh_key);

    // A definite no rather than a handle that never produces anything. The
    // evaluator has already logged which of the several possible reasons it was.
    if (!evaluator->isValid())
    {
        return kInvalidSignal;
    }

    auto found = impl_->by_key.find(key.zenoh_key);
    if (found == impl_->by_key.end())
    {
        auto subscription = std::make_unique<KeySubscription>();
        subscription->publish(std::make_shared<const BindingList>());

        KeySubscription* const raw = subscription.get();

        // By value: a steady_clock::time_point is trivially copyable, so the
        // callback needs no pointer back into Impl and cannot outlive it.
        const std::chrono::steady_clock::time_point t0 = impl_->t0;

        subscription->subscriber = std::make_unique<pub_sub::RawSubscriber>(
            key.zenoh_key,
            [raw, t0](const std::vector<std::uint8_t>& payload, std::string_view schema_name) {
                // One decode per bound signal rather than one per subscription,
                // because each expression may read different fields. Still one
                // *subscription* per key, which is the win: the wire traffic
                // and the zenoh-side bookkeeping are paid once.
                const std::shared_ptr<const BindingList> bindings = raw->snapshot();
                if (!bindings || bindings->empty())
                {
                    return;
                }

                // Read once, not once per binding. Every signal carried by this
                // message arrived at the same instant, and stamping them
                // separately would spread one sample across a few microseconds
                // of the axis for no reason -- and would make two signals from
                // the same message fail to line up under the shared cursor.
                const double t =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

                for (const std::shared_ptr<Binding>& binding : *bindings)
                {
                    // Latched inside the evaluator: one encoding comparison on
                    // the first sample, a predicted branch thereafter. Worth it
                    // because decoding against the wrong schema is silent --
                    // field offsets land on different bytes and you get a
                    // plausible number, not an error.
                    binding->evaluator->checkPublishedSchema(schema_name);

                    // nullopt means this sample was unusable. Dropping it
                    // leaves a gap in the line, which is honest; substituting
                    // zero would draw a spike that never happened.
                    if (const std::optional<double> value =
                            binding->evaluator->evaluateToDouble(payload))
                    {
                        binding->buffer->push(Sample{t, *value});
                    }
                }
            });

        if (!subscription->subscriber->isValid())
        {
            SPDLOG_ERROR("Failed to subscribe to '{}'.", key.zenoh_key);
            return kInvalidSignal;
        }

        found = impl_->by_key.emplace(key.zenoh_key, std::move(subscription)).first;
    }

    auto binding = std::make_shared<Binding>();
    binding->handle = impl_->next_handle++;
    binding->evaluator = std::move(evaluator);
    binding->buffer = std::move(into);

    KeySubscription* const subscription = found->second.get();
    auto updated = std::make_shared<BindingList>(*subscription->snapshot());
    updated->push_back(binding);
    subscription->publish(std::move(updated));

    impl_->handle_keys.emplace(binding->handle, key.zenoh_key);

    SPDLOG_DEBUG("Bound signal {} to '{}' ({}), expression '{}'.", binding->handle, key.zenoh_key,
                 reflection::enum_to_string(key.schema_type), key.value_expression);
    return binding->handle;
}

void LiveZenohSource::release(SignalHandle handle)
{
    const auto entry = impl_->handle_keys.find(handle);
    if (entry == impl_->handle_keys.end())
    {
        return;
    }

    const auto found = impl_->by_key.find(entry->second);
    if (found != impl_->by_key.end())
    {
        KeySubscription* const subscription = found->second.get();

        auto updated = std::make_shared<BindingList>(*subscription->snapshot());
        updated->erase(std::remove_if(updated->begin(), updated->end(),
                                      [handle](const std::shared_ptr<Binding>& binding)
                                      { return binding->handle == handle; }),
                       updated->end());

        if (updated->empty())
        {
            // Last signal on this key: drop the subscription rather than leave
            // it decoding samples nothing looks at. Erasing runs
            // ~KeySubscription, which tears down the subscriber first and joins
            // any callback still running.
            impl_->by_key.erase(found);
        }
        else
        {
            subscription->publish(std::move(updated));
        }
    }

    impl_->handle_keys.erase(entry);
}

double LiveZenohSource::now() const
{
    return impl_->secondsSince(std::chrono::steady_clock::now());
}

std::size_t LiveZenohSource::subscriptionCount() const
{
    return impl_->by_key.size();
}

}  // namespace scope
