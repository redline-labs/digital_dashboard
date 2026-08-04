// SPDX-License-Identifier: GPL-3.0-or-later
//
// Expression evaluation against a Cap'n Proto payload, with the emphasis on
// payloads a publisher should never send.
//
// The happy path is exercised every time anyone runs the dashboard. The cases
// that actually bit are the other ones, and every failure used to come back as
// 0.0 through the same channel as a real reading -- so a corrupt packet drove
// the gauge to zero and looked exactly like a genuine zero. On an oil-pressure
// or coolant gauge that is the worst available failure mode, which is why these
// now return nullopt and leave the last good value alone.
//
// Constructing a subscriber opens a zenoh session, so this is a `net` test and
// skips itself where no session can be opened, the same way the SessionManager
// test does.

#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_subscriber.h"

#include "engine_rpm.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A well-formed EngineRpm message, as a publisher would put it on the wire.
std::vector<uint8_t> engineRpmPayload(uint32_t rpm, float oil_psi = 0.0f)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<EngineRpm>();
    root.setTimestamp(0);
    root.setRpm(rpm);
    root.setOilPressurePsi(oil_psi);

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::unique_ptr<pub_sub::ZenohExpressionSubscriber> subscriberFor(const std::string& expression)
{
    return std::make_unique<pub_sub::ZenohExpressionSubscriber>(
        pub_sub::schema_type_t::EngineRpm, expression, "test/engine/rpm");
}

// ------------------------------------------------------------------ the basics

void testAWellFormedSampleEvaluates()
{
    auto sub = subscriberFor("rpm");
    expect(sub->isValid(), "a subscriber over a real field is valid");

    const auto value = sub->evaluate<float>(engineRpmPayload(4000));
    expect(value.has_value() && std::abs(*value - 4000.0f) < 0.001f,
           "a well-formed sample evaluates to the published value");

    auto arithmetic = subscriberFor("rpm / 1000.0");
    const auto scaled = arithmetic->evaluate<float>(engineRpmPayload(6000));
    expect(scaled.has_value() && std::abs(*scaled - 6.0f) < 0.001f,
           "arithmetic in an expression is applied");
}

void testIntegerAndBooleanResults()
{
    auto sub = subscriberFor("rpm");

    const auto as_int = sub->evaluate<int>(engineRpmPayload(3999));
    expect(as_int.has_value() && *as_int == 3999, "an integer result comes back exact");

    auto predicate = subscriberFor("rpm > 5000");
    expect(predicate->evaluate<bool>(engineRpmPayload(6000)).value_or(false),
           "a predicate above the threshold is true");
    expect(predicate->evaluate<bool>(engineRpmPayload(1000)).has_value() &&
               !predicate->evaluate<bool>(engineRpmPayload(1000)).value(),
           "a predicate below the threshold is false, not absent");
}

// ------------------------------------------------------- malformed payloads

void testTruncatedAndMisalignedPayloadsAreRejected()
{
    auto sub = subscriberFor("rpm");
    const std::vector<uint8_t> good = engineRpmPayload(4000);

    // Under one capnp word. This used to decode as an empty message -- every
    // field its default -- and report a confident 0 with no log line at all.
    expect(!sub->evaluate<float>(std::vector<uint8_t>{1, 2, 3}).has_value(),
           "a payload shorter than one capnp word is rejected, not read as zero");

    expect(!sub->evaluate<float>(std::vector<uint8_t>{}).has_value(),
           "an empty payload is rejected");

    // Not a whole number of words: the length was divided by 8 and the
    // remainder silently dropped.
    std::vector<uint8_t> ragged = good;
    ragged.push_back(0xAB);
    expect(!sub->evaluate<float>(ragged).has_value(),
           "a payload that is not a whole number of words is rejected");

    // Word-aligned but structurally wrong. capnp throws here; the throw must not
    // escape, and must not turn into a reading either.
    const std::vector<uint8_t> garbage(64, 0xFF);
    expect(!sub->evaluate<float>(garbage).has_value(),
           "word-aligned garbage is rejected rather than throwing or reading as zero");

    // Truncated mid-message, which is what a severed transfer looks like.
    std::vector<uint8_t> half(good.begin(), good.begin() + (good.size() / 2 / 8) * 8);
    (void)sub->evaluate<float>(half);  // may or may not decode; must not crash

    // The subscriber still works afterwards: a bad sample must not poison it.
    const auto recovered = sub->evaluate<float>(good);
    expect(recovered.has_value() && std::abs(*recovered - 4000.0f) < 0.001f,
           "a good sample after a bad one still evaluates");
}

// ------------------------------------------------------- non-finite results

void testNonFiniteResultsAreRejected()
{
    // exprtk does plain IEEE division: 0/0 is NaN and x/0 is inf, with no throw
    // and no flag. std::clamp does not filter NaN and static_cast<int>(NaN) is
    // undefined, so both had to be stopped here.
    auto nan_expr = subscriberFor("(rpm - rpm) / (rpm - rpm)");
    expect(!nan_expr->evaluate<float>(engineRpmPayload(4000)).has_value(),
           "a NaN result is rejected rather than delivered");

    auto inf_expr = subscriberFor("1.0 / (rpm - rpm)");
    expect(!inf_expr->evaluate<float>(engineRpmPayload(4000)).has_value(),
           "an infinite result is rejected rather than delivered");

    // The integer path is the one where a non-finite value was undefined
    // behaviour rather than merely wrong.
    expect(!nan_expr->evaluate<int>(engineRpmPayload(4000)).has_value(),
           "a NaN result is rejected on the integer path too");
    expect(!inf_expr->evaluate<int>(engineRpmPayload(4000)).has_value(),
           "an infinite result is rejected on the integer path too");
}

void testOutOfRangeIntegerResultsAreRejected()
{
    // Casting a double outside the destination's range is undefined, not
    // saturating, so it has to be refused before the cast.
    auto huge = subscriberFor("rpm * 1.0e18");
    expect(!huge->evaluate<int>(engineRpmPayload(4000)).has_value(),
           "a result too large for the destination integer is rejected");

    auto negative = subscriberFor("0 - rpm * 1.0e18");
    expect(!negative->evaluate<int>(engineRpmPayload(4000)).has_value(),
           "a result too negative for the destination integer is rejected");

    // ...but a value that does fit still gets through.
    auto fits = subscriberFor("rpm");
    expect(fits->evaluate<int>(engineRpmPayload(4000)).value_or(0) == 4000,
           "a result that fits is still delivered");
}

// ------------------------------------------------------ construction failures

void testInvalidExpressionsAreRejectedAtConstruction()
{
    // A variable that is not a field of the schema.
    auto unknown_field = subscriberFor("notAField");
    expect(!unknown_field->isValid(), "an expression over a non-existent field is invalid");
    expect(!unknown_field->evaluate<float>(engineRpmPayload(4000)).has_value(),
           "an invalid subscriber evaluates to nothing");

    // Syntactically broken.
    auto broken = subscriberFor("rpm +");
    expect(!broken->isValid(), "a syntactically invalid expression is invalid");

    // An empty key or expression.
    pub_sub::ZenohExpressionSubscriber empty_expression(
        pub_sub::schema_type_t::EngineRpm, "", "test/engine/rpm");
    expect(!empty_expression.isValid(), "an empty expression is invalid");

    pub_sub::ZenohExpressionSubscriber empty_key(
        pub_sub::schema_type_t::EngineRpm, "rpm", "");
    expect(!empty_key.isValid(), "an empty zenoh key is invalid");
}

void testRegisteredUnitHelpersAreUsable()
{
    // These are registered as exprtk functions, so they must not be mistaken for
    // schema fields during validation.
    auto converted = subscriberFor("psi_to_bar(oilPressurePsi)");
    expect(converted->isValid(), "an expression using a registered helper is valid");

    const auto value = converted->evaluate<float>(engineRpmPayload(4000, 14.5038f));
    expect(value.has_value() && std::abs(*value - 1.0f) < 0.01f,
           "a registered unit helper is applied");
}

}  // namespace

int main()
{
    // A subscriber needs a session. On a host where none can be opened this is
    // testing the environment, so say so and skip rather than fail.
    if (!pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_WARN("No zenoh session available; skipping expression evaluation tests.");
        return 0;
    }

    testAWellFormedSampleEvaluates();
    testIntegerAndBooleanResults();
    testTruncatedAndMisalignedPayloadsAreRejected();
    testNonFiniteResultsAreRejected();
    testOutOfRangeIntegerResultsAreRejected();
    testInvalidExpressionsAreRejectedAtConstruction();
    testRegisteredUnitHelpersAreUsable();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
