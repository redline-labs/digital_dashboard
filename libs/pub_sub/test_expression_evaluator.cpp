// SPDX-License-Identifier: GPL-3.0-or-later
//
// ExpressionEvaluator on its own: decode a Cap'n Proto payload against a
// schema, evaluate an expression over its fields, convert the result.
//
// This is the same behaviour test_expression_eval.cpp covers through
// ZenohExpressionSubscriber, and that test stays exactly as it was -- it is the
// evidence that pulling the evaluator out of the subscriber changed nothing.
// What is different here is that no session is opened, so this is a `unit` test
// rather than a `net` one and cannot skip itself on a machine with no bus.
// Being able to test the decode path with nothing on the other end is most of
// why the split was worth doing.
//
// Weighted towards payloads a publisher should never send. The happy path is
// exercised every time anyone runs the dashboard; the cases that actually bit
// are the other ones, and every failure used to come back as 0.0 through the
// same channel as a real reading -- so a corrupt packet drove a gauge to zero
// and looked exactly like a genuine zero.

#include "pub_sub/expression_evaluator.h"
#include "pub_sub/schema_registry.h"

#include "engine_rpm.capnp.h"
#include "vehicle_speed.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
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

pub_sub::ExpressionEvaluator evaluatorFor(const std::string& expression)
{
    return pub_sub::ExpressionEvaluator(pub_sub::schema_type_t::EngineRpm, expression,
                                        "test/engine/rpm");
}

// ------------------------------------------------------------------ the basics

void testAWellFormedSampleEvaluates()
{
    auto eval = evaluatorFor("rpm");
    expect(eval.isValid(), "an evaluator over a real field is valid");

    const auto value = eval.evaluate<double>(engineRpmPayload(4000));
    expect(value.has_value() && std::abs(*value - 4000.0) < 1e-9,
           "a well-formed sample evaluates to the published value");
}

void testArithmeticIsApplied()
{
    auto eval = evaluatorFor("rpm / 1000.0");
    const auto value = eval.evaluate<double>(engineRpmPayload(4500));
    expect(value.has_value() && std::abs(*value - 4.5) < 1e-9,
           "arithmetic in the expression is applied to the decoded field");
}

void testSeveralFieldsInOneExpression()
{
    auto eval = evaluatorFor("rpm / 100.0 + oilPressurePsi");
    const auto value = eval.evaluate<double>(engineRpmPayload(3000, 12.5f));
    expect(value.has_value() && std::abs(*value - 42.5) < 1e-6,
           "an expression reading two fields sees both of them");
}

void testUnitConversionHelpersAreAvailable()
{
    auto eval = evaluatorFor("psi_to_bar(oilPressurePsi)");
    const auto value = eval.evaluate<double>(engineRpmPayload(0, 14.5038f));
    expect(value.has_value() && std::abs(*value - 1.0) < 1e-3,
           "the registered unit-conversion functions are callable from an expression");
}

// ----------------------------------------------------- construction is checked

void testAnUnknownFieldIsRejected()
{
    auto eval = evaluatorFor("thisFieldDoesNotExist");
    expect(!eval.isValid(), "a variable that is not a field of the schema is a construction error");
    expect(!eval.evaluate<double>(engineRpmPayload(1000)).has_value(),
           "an invalid evaluator produces no value rather than a plausible zero");
}

void testANonNumericFieldIsRejected()
{
    // VehicleSpeed carries a Text field; an expression cannot turn text into a
    // number, and that is knowable at construction rather than once per sample
    // forever. Previously this warned at the sample rate and substituted 0.0.
    const auto schema = pub_sub::get_schema(pub_sub::schema_type_t::VehicleSpeed);
    if (!schema)
    {
        return;  // Schema list changed; nothing to assert here.
    }

    bool found_text_field = false;
    std::string text_field_name;
    for (auto field : schema->asStruct().getFields())
    {
        if (field.getType().isText())
        {
            found_text_field = true;
            text_field_name = field.getProto().getName().cStr();
            break;
        }
    }

    if (!found_text_field)
    {
        return;  // No text field to point at; the INT/UINT/FLOAT path is covered above.
    }

    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::VehicleSpeed, text_field_name,
                                      "test/vehicle/speed");
    expect(!eval.isValid(), "a non-numeric field is rejected at construction, not per sample");
}

void testAnEmptyExpressionIsRejected()
{
    auto eval = evaluatorFor("");
    expect(!eval.isValid(), "an empty expression is a construction error");
}

void testAMalformedExpressionIsRejected()
{
    auto eval = evaluatorFor("rpm +* 3");
    expect(!eval.isValid(), "an expression that does not parse is a construction error");
}

// --------------------------------------------------------- malformed payloads

void testAnEmptyPayloadProducesNoValue()
{
    auto eval = evaluatorFor("rpm");
    expect(!eval.evaluate<double>({}).has_value(),
           "an empty payload produces no value rather than zero");
}

void testAPartialWordPayloadProducesNoValue()
{
    // capnp reads whole 8-byte words. A payload that is not a multiple of
    // sizeof(word) used to be silently truncated, and anything under one word
    // decoded as an empty message -- every field its default, no warning.
    auto eval = evaluatorFor("rpm");

    std::vector<uint8_t> payload = engineRpmPayload(4000);
    payload.pop_back();  // No longer a whole number of words.

    expect(!eval.evaluate<double>(payload).has_value(),
           "a payload that is not a whole number of capnp words produces no value");
}

void testATruncatedPayloadProducesNoValue()
{
    auto eval = evaluatorFor("rpm");

    std::vector<uint8_t> payload = engineRpmPayload(4000);
    // Drop a whole word, so it stays aligned but the message is incomplete.
    payload.resize(payload.size() - sizeof(capnp::word));

    expect(!eval.evaluate<double>(payload).has_value(),
           "a payload truncated by a whole word produces no value");
}

void testGarbageProducesNoValueOrANumberButNeverThrows()
{
    auto eval = evaluatorFor("rpm");

    // Word-aligned nonsense. capnp may or may not reject this; what matters is
    // that nothing escapes, because the caller is a zenoh callback and an
    // exception crossing back into Rust aborts the process.
    const std::vector<uint8_t> payload(sizeof(capnp::word) * 4, 0xAB);

    bool threw = false;
    try
    {
        (void)eval.evaluate<double>(payload);
    }
    catch (...)
    {
        threw = true;
    }
    expect(!threw, "garbage input never escapes as an exception");
}

// ------------------------------------------------- non-finite and out of range

void testDivisionByZeroProducesNoValue()
{
    // exprtk does plain IEEE division, so x/0 is inf with no throw and no flag.
    // inf then poisons whatever it touches -- static_cast<int>(inf) is undefined
    // behaviour, and std::clamp passes it straight through to painter.rotate().
    auto eval = evaluatorFor("rpm / (rpm - rpm)");
    expect(!eval.evaluate<double>(engineRpmPayload(4000)).has_value(),
           "an expression evaluating to infinity produces no value");
}

void testNotANumberProducesNoValue()
{
    auto eval = evaluatorFor("(rpm - rpm) / (rpm - rpm)");
    expect(!eval.evaluate<double>(engineRpmPayload(4000)).has_value(),
           "an expression evaluating to NaN produces no value");
}

void testAResultThatDoesNotFitTheDestinationProducesNoValue()
{
    // Casting a double outside the destination's range is undefined, not
    // saturating, so the conversion has to check before it casts.
    auto eval = evaluatorFor("rpm * 1000000.0");
    expect(!eval.evaluate<int16_t>(engineRpmPayload(4000)).has_value(),
           "a result too large for the destination integer type produces no value");

    auto negative = evaluatorFor("0 - rpm");
    expect(!negative.evaluate<uint16_t>(engineRpmPayload(4000)).has_value(),
           "a negative result produces no value for an unsigned destination "
           "rather than wrapping to a plausible one");
}

void testAResultAtTheEdgeOfTheDestinationStillFits()
{
    auto eval = evaluatorFor("rpm");
    const auto value = eval.evaluate<uint16_t>(engineRpmPayload(65535));
    expect(value.has_value() && *value == 65535,
           "a result exactly at the destination's maximum is accepted");
}

void testConversionRounds()
{
    auto eval = evaluatorFor("rpm / 3.0");
    const auto value = eval.evaluate<int32_t>(engineRpmPayload(10));
    // 10/3 == 3.333..., which rounds to 3 rather than truncating to 3 by luck.
    expect(value.has_value() && *value == 3, "an integer conversion rounds rather than truncates");

    auto up = evaluatorFor("rpm / 4.0");
    const auto rounded_up = up.evaluate<int32_t>(engineRpmPayload(10));
    // 10/4 == 2.5, which rounds away from zero to 3.
    expect(rounded_up.has_value() && *rounded_up == 3,
           "an integer conversion rounds 2.5 up rather than down");
}

void testBoolConversionIsNonZero()
{
    auto eval = evaluatorFor("rpm");
    const auto set = eval.evaluate<bool>(engineRpmPayload(1));
    expect(set.has_value() && *set, "a non-zero result converts to true");

    const auto clear = eval.evaluate<bool>(engineRpmPayload(0));
    expect(clear.has_value() && !*clear, "a zero result converts to false");
}

// --------------------------------------------------------------- introspection

void testVariableNamesAreReported()
{
    auto eval = evaluatorFor("oilPressurePsi + rpm + rpm");
    const auto& names = eval.variableNames();

    expect(names.size() == 2, "each variable is reported once however often it appears");
    // std::map iterates sorted, which is where the ordering guarantee comes from.
    expect(names.size() == 2 && names[0] == "oilPressurePsi" && names[1] == "rpm",
           "variable names come back in sorted order");
}

void testSchemaAndExpressionAreReportedBack()
{
    auto eval = evaluatorFor("rpm / 2");
    expect(eval.getSchemaType() == pub_sub::schema_type_t::EngineRpm,
           "the configured schema is reported back");
    expect(eval.getExpression() == "rpm / 2", "the expression is reported back verbatim");
}

void testCheckPublishedSchemaToleratesAnything()
{
    // The mismatch is only reportable, never fatal: decoding against the wrong
    // schema still yields a number, and a wrong reading with a loud log line
    // beats a blank one. What matters here is that none of these throw, since
    // this runs from a zenoh callback.
    auto eval = evaluatorFor("rpm");

    bool threw = false;
    try
    {
        eval.checkPublishedSchema("application/capnp;EngineRpm");   // Matching.
        eval.checkPublishedSchema("application/capnp;VehicleSpeed");  // Latched, so ignored.

        auto other = evaluatorFor("rpm");
        other.checkPublishedSchema("application/capnp;VehicleSpeed");  // Mismatch.

        auto no_schema = evaluatorFor("rpm");
        no_schema.checkPublishedSchema("application/capnp");  // MIME only.

        auto empty = evaluatorFor("rpm");
        empty.checkPublishedSchema("");  // Nothing at all.
    }
    catch (...)
    {
        threw = true;
    }
    expect(!threw, "the schema check never throws, whatever encoding it is handed");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testAWellFormedSampleEvaluates();
    testArithmeticIsApplied();
    testSeveralFieldsInOneExpression();
    testUnitConversionHelpersAreAvailable();

    testAnUnknownFieldIsRejected();
    testANonNumericFieldIsRejected();
    testAnEmptyExpressionIsRejected();
    testAMalformedExpressionIsRejected();

    testAnEmptyPayloadProducesNoValue();
    testAPartialWordPayloadProducesNoValue();
    testATruncatedPayloadProducesNoValue();
    testGarbageProducesNoValueOrANumberButNeverThrows();

    testDivisionByZeroProducesNoValue();
    testNotANumberProducesNoValue();
    testAResultThatDoesNotFitTheDestinationProducesNoValue();
    testAResultAtTheEdgeOfTheDestinationStillFits();
    testConversionRounds();
    testBoolConversionIsNonZero();

    testVariableNamesAreReported();
    testSchemaAndExpressionAreReportedBack();
    testCheckPublishedSchemaToleratesAnything();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
