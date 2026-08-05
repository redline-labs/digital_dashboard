#include "pub_sub/expression_subscriber.h"

#include "pub_sub/detail/byte_subscriber.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace pub_sub
{

struct ZenohExpressionSubscriber::Impl
{
    std::string zenoh_key;

    // The evaluator this subscription feeds. Owned by the enclosing object, not
    // by Impl -- Impl only borrows it, and outlives nothing.
    ExpressionEvaluator* evaluator = nullptr;

    // Set once by setRawCallback(), read on every sample from a zenoh thread.
    //
    // Published with release/acquire rather than guarded by a mutex. The
    // subscription is live before the callback is installed -- isValid() has to
    // mean "this will deliver data", so it cannot wait for a callback that may
    // never come -- which leaves a startup window where a sample can arrive first.
    // Reading a std::function while it is being assigned is a data race; a mutex
    // would close it at the cost of a lock on every sample forever, to serialise
    // against a write that happens once. `handler` is written before the release
    // store and only read after the acquire load, and never reassigned.
    std::function<void(const std::vector<std::uint8_t>&)> handler;
    std::atomic<bool> handler_ready{false};

    bool subscription_valid = false;

    // Declared last so it is destroyed FIRST. zenoh's undeclare joins in-flight
    // callbacks, so by the time the members above are destroyed no callback can
    // still be touching them. This ordering was a use-after-free on every
    // teardown, and on every rebuildWidget() from the agent's set_config path,
    // before it was worked out -- do not move it.
    std::unique_ptr<detail::ByteSubscriber> subscriber;
};

ZenohExpressionSubscriber::ZenohExpressionSubscriber(schema_type_t schema_type,
                                                     const std::string& expression,
                                                     const std::string& zenoh_key) :
    evaluator_(std::make_unique<ExpressionEvaluator>(schema_type, expression, zenoh_key)),
    impl_(std::make_unique<Impl>())
{
    impl_->zenoh_key = zenoh_key;
    impl_->evaluator = evaluator_.get();

    // An empty key is this class's own precondition rather than the
    // evaluator's: the evaluator is perfectly usable with no key at all, since
    // a recorded source supplies bytes from somewhere else entirely.
    if (zenoh_key.empty())
    {
        SPDLOG_ERROR("Zenoh key is empty for expression '{}'", expression);
        return;
    }

    Impl* const impl = impl_.get();
    impl_->subscriber = std::make_unique<detail::ByteSubscriber>(
        zenoh_key,
        [impl](const std::vector<std::uint8_t>& payload, const detail::SampleMeta& meta) {
            if (!impl->handler_ready.load(std::memory_order_acquire))
            {
                return;
            }
            // Latched inside the evaluator, so this costs one encoding copy on
            // the first sample and a predicted branch thereafter.
            impl->evaluator->checkPublishedSchema(meta.encoding());
            impl->handler(payload);
        });

    impl_->subscription_valid = impl_->subscriber->isValid();
}

ZenohExpressionSubscriber::~ZenohExpressionSubscriber() = default;

bool ZenohExpressionSubscriber::isValid() const
{
    // Both halves. Leaving this true when the subscription failed handed callers
    // a subscriber that passed validation and then sat silent forever.
    return evaluator_->isValid() && impl_->subscription_valid;
}

schema_type_t ZenohExpressionSubscriber::getSchemaType() const
{
    return evaluator_->getSchemaType();
}

const std::string& ZenohExpressionSubscriber::getExpression() const
{
    return evaluator_->getExpression();
}

void ZenohExpressionSubscriber::setRawCallback(
    std::function<void(const std::vector<std::uint8_t>&)> handler)
{
    impl_->handler = std::move(handler);
    // Release: everything written to `handler` above is visible to any zenoh
    // thread that sees this store. See Impl::handler_ready.
    impl_->handler_ready.store(true, std::memory_order_release);
}

void ZenohExpressionSubscriber::reportCallbackThrew()
{
    SPDLOG_ERROR("Evaluation callback threw for key '{}'.", impl_->zenoh_key);
}

}  // namespace pub_sub
