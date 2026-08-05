#ifndef PUB_SUB_EXPRESSION_SUBSCRIBER_H_
#define PUB_SUB_EXPRESSION_SUBSCRIBER_H_

#include "pub_sub/schema_registry.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
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
// Variables in the expression are field names of the schema, validated against it
// at construction, so a typo is a startup error rather than a gauge that reads
// zero forever.
//
// Everything this needs -- zenoh, exprtk, capnp's dynamic API -- is behind Impl.
// That is not tidiness: this header is included, directly or through
// dashboard/expression_subscription.h, by 32 translation units, and inlining
// exprtk's symbol table and parser into it cost every one of them 135,000
// preprocessed lines of a library they never name.
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
        // Everything that needs capnp or exprtk happens in here, out of line.
        // What is left is the conversion to T, which is plain arithmetic and the
        // only part that has to be a template.
        const std::optional<double> result = evaluateToDouble(payload);
        if (!result)
        {
            return std::nullopt;
        }

        if constexpr (std::is_same_v<T, bool>)
        {
            // Anything non-zero is true.
            return static_cast<T>(*result != 0.0);
        }
        else if constexpr (std::is_integral_v<T>)
        {
            // Round first, then check the value actually fits: casting a double
            // outside the destination's range is undefined, not saturating.
            const double rounded = std::round(*result);
            if (rounded < static_cast<double>(std::numeric_limits<T>::lowest()) ||
                rounded > static_cast<double>(std::numeric_limits<T>::max()))
            {
                warnOutOfRange(rounded);
                return std::nullopt;
            }
            return static_cast<T>(rounded);
        }
        else
        {
            return static_cast<T>(*result);
        }
    }

  private:
    // The decode-and-evaluate half, out of line because it is what drags in
    // capnp's dynamic API and exprtk. Returns nullopt for an unusable sample,
    // having already logged whatever needed logging (latched, so a malformed
    // publisher does not churn the log at the sample rate).
    std::optional<double> evaluateToDouble(const std::vector<std::uint8_t>& payload);

    // Latched, and needs the key and expression for its message, so it cannot be
    // in the template above.
    void warnOutOfRange(double value);
    void reportCallbackThrew();

    void setRawCallback(std::function<void(const std::vector<std::uint8_t>&)> handler);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_EXPRESSION_SUBSCRIBER_H_
