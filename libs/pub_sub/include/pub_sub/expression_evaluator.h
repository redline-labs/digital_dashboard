#ifndef PUB_SUB_EXPRESSION_EVALUATOR_H_
#define PUB_SUB_EXPRESSION_EVALUATOR_H_

#include "pub_sub/schema_registry.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pub_sub
{

// "Decode these bytes against this schema and evaluate this expression over its
// fields" -- with no zenoh in it at all.
//
//   ExpressionEvaluator eval(schema_type_t::EngineRpm, "rpm / 1000.0");
//   const std::optional<double> krpm = eval.evaluate<double>(payload);
//
// Variables in the expression are field names of the schema, validated against
// it at construction, so a typo is a startup error rather than a value that
// reads zero forever.
//
// This was the inside of ZenohExpressionSubscriber, which welded it to a live
// subscription. Two things need it without one. A recorded-data source replays
// payloads that arrived from a file, and has no subscription to speak of. And a
// consumer plotting ten fields of one topic wants to subscribe once and
// evaluate ten expressions against each sample, rather than open ten
// subscriptions that each decode the same message.
//
// Everything this needs -- exprtk, capnp's dynamic API -- is behind Impl. That
// is not tidiness: this header is reached, directly or indirectly, by every
// translation unit that binds a widget or a panel to a signal, and inlining
// exprtk's symbol table and parser cost every one of them 135,000 preprocessed
// lines of a library they never name.
class ExpressionEvaluator
{
  public:
    // `log_context` is what messages name when reporting a bad sample -- the
    // zenoh key, normally. It is only ever used in log text; pass anything that
    // will mean something to whoever reads the log.
    ExpressionEvaluator(schema_type_t schema_type,
                        const std::string& expression,
                        std::string log_context = {});
    ~ExpressionEvaluator();

    // Field slots are bound into exprtk's symbol table by address, so the
    // addresses have to stay put.
    ExpressionEvaluator(const ExpressionEvaluator&) = delete;
    ExpressionEvaluator& operator=(const ExpressionEvaluator&) = delete;
    ExpressionEvaluator(ExpressionEvaluator&&) = delete;
    ExpressionEvaluator& operator=(ExpressionEvaluator&&) = delete;

    // True when the expression compiled and every variable resolved to a
    // numeric field of the schema. Says nothing about where bytes come from --
    // that is the caller's problem, and ZenohExpressionSubscriber's isValid()
    // folds its subscription state in on top of this.
    bool isValid() const;

    schema_type_t getSchemaType() const;
    const std::string& getExpression() const;

    // The schema field names the expression reads, in sorted order. Useful to a
    // consumer that wants to show what a binding actually depends on.
    const std::vector<std::string>& variableNames() const;

    // Compares the schema this was configured for against the one a publisher
    // stamped on a sample, and complains loudly, once, if they differ.
    //
    // Worth calling on the first sample of every stream. capnp will decode a
    // payload against whatever schema it is handed -- field offsets simply land
    // on different bytes -- so a wrong schema produces a plausible but
    // meaningless number rather than an error. This is the only place the
    // mismatch is detectable at all.
    //
    // Takes the whole encoding string ("application/capnp;EngineRpm"), not the
    // schema half, so it can tell "published as something else" apart from
    // "published with no schema named", which are different situations and
    // deserve different messages. Latched: a malformed publisher would
    // otherwise churn the log at the sample rate.
    void checkPublishedSchema(std::string_view encoding);

    // Evaluate the expression against one payload.
    //
    // Returns nullopt when this sample produced no usable number -- deliberately
    // a different outcome from "the value is zero". Every failure here used to
    // return 0.0, which drove gauges to zero on a corrupt packet; for an
    // oil-pressure or coolant gauge, reading zero because a packet was damaged
    // is the worst available failure.
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

    // The decode-and-evaluate half, out of line because it is what drags in
    // capnp's dynamic API and exprtk. Returns nullopt for an unusable sample,
    // having already logged whatever needed logging (latched, so a malformed
    // publisher does not churn the log at the sample rate).
    //
    // Public because a consumer that only ever wants a double has no reason to
    // go through the template.
    std::optional<double> evaluateToDouble(const std::vector<std::uint8_t>& payload);

  private:
    // Latched, and needs the context and expression for its message, so it
    // cannot live in the template above.
    void warnOutOfRange(double value);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_EXPRESSION_EVALUATOR_H_
