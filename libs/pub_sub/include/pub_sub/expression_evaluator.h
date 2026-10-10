#ifndef PUB_SUB_EXPRESSION_EVALUATOR_H_
#define PUB_SUB_EXPRESSION_EVALUATOR_H_

#include "pub_sub/schema_registry.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

    // Whether this key's publisher may be evaluated at all: the schema name it
    // stamped and its revision, judged against the configured schema by
    // SampleGate. capnp decodes a payload against whatever schema it is handed,
    // so a mismatch would read as plausible wrong numbers; it is refused and said
    // once instead.
    //
    // `published_name` is the bare name, as RawSubscriber and BagMessage carry
    // it ("" when none was stamped). Judged on the first call and latched: the
    // verdict is returned unchanged after that, so a caller with a costly way to
    // build the arguments asks publisherVerdict() first.
    bool admitsPublisher(std::string_view published_name, std::optional<std::uint64_t> layout);

    // The latched verdict, or nullopt before the first admitsPublisher().
    std::optional<bool> publisherVerdict() const;

    // Evaluate the expression against one payload.
    //
    // Returns nullopt when this sample produced no usable number -- deliberately
    // a different outcome from "the value is zero". Every failure here used to
    // return 0.0, which drove gauges to zero on a corrupt packet; for an
    // oil-pressure or coolant gauge, reading zero because a packet was damaged
    // is the worst available failure.
    template <typename T>
    std::optional<T> evaluate(std::span<const std::uint8_t> payload)
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
    //
    // A SPAN, not a vector. A live subscriber does hand over a fresh
    // std::vector from zenoh's as_vector(), but a recorded source does not: a
    // bag::BagMessage's payload is a view into a decompressed chunk the reader
    // owns. Taking a vector forced a copy of every payload on the replay path
    // -- per message, per bound signal -- for no reason at all, since
    // WordAlignedPayload has taken (pointer, size) all along.
    std::optional<double> evaluateToDouble(std::span<const std::uint8_t> payload);

  private:
    // Latched, and needs the context and expression for its message, so it
    // cannot live in the template above.
    void warnOutOfRange(double value);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_EXPRESSION_EVALUATOR_H_
