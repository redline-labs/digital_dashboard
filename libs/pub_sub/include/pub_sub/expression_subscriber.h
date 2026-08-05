#ifndef PUB_SUB_EXPRESSION_SUBSCRIBER_H_
#define PUB_SUB_EXPRESSION_SUBSCRIBER_H_

#include "pub_sub/expression_evaluator.h"
#include "pub_sub/schema_registry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pub_sub
{

// Subscribes to a key, decodes each sample against a schema, and evaluates an
// arithmetic expression over its fields -- the config-driven path a dashboard
// widget uses:
//
//   ZenohExpressionSubscriber sub(schema_type_t::EngineRpm, "rpm / 1000.0",
//                                "vehicle/engine/rpm");
//   sub.setResultCallback<float>([](float krpm) { ... });
//
// Variables in the expression are field names of the schema, validated against
// it at construction, so a typo is a startup error rather than a gauge that reads
// zero forever.
//
// This is now a thin join of two pieces: an ExpressionEvaluator, which is all
// the decoding and arithmetic, and a subscription that feeds it bytes. The
// evaluator is separately usable, which is what lets a consumer subscribe once
// per key and evaluate several expressions per sample, and what lets a
// recorded-data source reuse the decode path with no bus at all.
class ZenohExpressionSubscriber
{
  public:
    // `expression` is arithmetic over the schema's numeric fields; `zenoh_key` is
    // subscribed immediately via the process-wide SessionManager.
    ZenohExpressionSubscriber(schema_type_t schema_type,
                              const std::string& expression,
                              const std::string& zenoh_key);
    ~ZenohExpressionSubscriber();

    // The subscription's callback holds a pointer to Impl, so the address has to
    // stay put.
    ZenohExpressionSubscriber(const ZenohExpressionSubscriber&) = delete;
    ZenohExpressionSubscriber& operator=(const ZenohExpressionSubscriber&) = delete;
    ZenohExpressionSubscriber(ZenohExpressionSubscriber&&) = delete;
    ZenohExpressionSubscriber& operator=(ZenohExpressionSubscriber&&) = delete;

    // True only when this will actually deliver data: the expression compiled,
    // every variable resolved to a numeric field of the schema, AND the
    // subscription was declared. It used to mean only the first of those, which
    // handed callers a subscriber that passed validation and then sat silent.
    bool isValid() const;

    schema_type_t getSchemaType() const;
    const std::string& getExpression() const;

    // Deliver evaluated results as T. Call at most once.
    //
    // The callback runs on a zenoh RX thread, not the caller's. Widgets do not
    // use this directly -- dashboard::ExpressionSubscription wraps it to coalesce
    // samples and hand them to the GUI thread.
    template <typename T>
    void setResultCallback(std::function<void(T)> callback)
    {
        setRawCallback([this, cb = std::move(callback)](const std::vector<std::uint8_t>& payload) {
            try
            {
                // No value means this sample was unusable (bad payload,
                // non-finite result, out of range). Skip the callback entirely so
                // the consumer keeps showing its last good reading rather than
                // snapping to zero.
                if (const std::optional<T> value = this->evaluate<T>(payload))
                {
                    cb(*value);
                }
            }
            catch (...)
            {
                // Nothing may escape into a zenoh callback: this unwinds through
                // the Rust FFI boundary, which aborts. kj/capnp throws do derive
                // from std::exception, but catching everything is free insurance
                // against the one that does not.
                reportCallbackThrew();
            }
        });
    }

    // Evaluate the expression against one payload.
    //
    // Returns nullopt when this sample produced no usable number -- deliberately
    // a different outcome from "the value is zero". Every failure here used to
    // return 0.0, which drove gauges to zero on a corrupt packet; for an
    // oil-pressure or coolant gauge, reading zero because a packet was damaged is
    // the worst available failure.
    //
    // Public because "evaluate this expression against these bytes" is what the
    // class is for, and because the failure modes it has to get right -- a
    // truncated payload, a division by zero, a result that does not fit the
    // destination -- are precisely what wants testing without a live publisher on
    // the other end of a bus.
    template <typename T>
    std::optional<T> evaluate(const std::vector<std::uint8_t>& payload)
    {
        return evaluator_->evaluate<T>(payload);
    }

  private:
    void reportCallbackThrew();

    void setRawCallback(std::function<void(const std::vector<std::uint8_t>&)> handler);

    // Declared BEFORE impl_, so it is destroyed AFTER it. impl_ owns the
    // subscription, whose destructor joins in-flight callbacks, and those
    // callbacks reach into the evaluator -- so the evaluator has to still be
    // alive while they drain. Reversing these two is a use-after-free on every
    // teardown, which is the same lesson that fixed the member order inside the
    // old Impl. Do not move it.
    //
    // A unique_ptr rather than a value member because ExpressionEvaluator is
    // neither copyable nor movable: exprtk binds to the addresses of its field
    // slots.
    std::unique_ptr<ExpressionEvaluator> evaluator_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_EXPRESSION_SUBSCRIBER_H_
