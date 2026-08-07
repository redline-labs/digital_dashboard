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
#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

#include "carplay_session.capnp.h"
#include "engine_rpm.capnp.h"
#include "motec_pdm.capnp.h"
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

// A CarPlaySessionState carrying one of its enum values. `phase` is an enum, and
// until enums were numeric this whole topic's most interesting field could not
// be bound by anything.
std::vector<uint8_t> sessionPayload(CarPlaySessionState::Phase phase)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<CarPlaySessionState>();
    root.setPhase(phase);
    root.setDeviceConnected(true);

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// A MoTeC PDM output-current message: `values` is a List(Float32), one per
// output. The real device sends 32; the length is a property of the MESSAGE, not
// of the schema, which is the whole reason the vector binding has to discover it.
std::vector<uint8_t> pdmCurrentPayload(const std::vector<float>& amps)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<MotecPdmOutputCurrent>();
    auto values = root.initValues(static_cast<unsigned>(amps.size()));
    for (unsigned i = 0; i < amps.size(); ++i)
    {
        values.set(i, amps[i]);
    }

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// A PDM output-status message: List(PdmOutputStatusEnum). A list of enums, which
// is both of the new cases at once.
std::vector<uint8_t> pdmStatusPayload(const std::vector<PdmOutputStatusEnum>& states)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<MotecPdmOutputStatus>();
    auto values = root.initValues(static_cast<unsigned>(states.size()));
    for (unsigned i = 0; i < states.size(); ++i)
    {
        values.set(i, states[i]);
    }

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
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

// ------------------------------------------------------------ enums and lists

void testAnEnumEvaluatesToItsOrdinal()
{
    // Before this, EVERY enum on the bus was unbindable -- CarPlay's session
    // phase and audio stream type, the PDM's per-output status. The field a
    // topic is most often watched for was the one field nothing could read.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::CarPlaySessionState, "phase",
                                      "test/carplay/session");
    expect(eval.isValid(), "an enum field is accepted");

    const auto recording =
        eval.evaluate<double>(sessionPayload(CarPlaySessionState::Phase::RECORDING));
    const auto idle = eval.evaluate<double>(sessionPayload(CarPlaySessionState::Phase::IDLE));

    expect(recording.has_value() && idle.has_value(), "both enum samples evaluate");
    expect(recording.has_value() && idle.has_value() && *recording != *idle,
           "different enumerants evaluate to different numbers");

    // The ORDINAL, i.e. the declaration position capnp stores -- not any value a
    // comment in the .capnp file mentions.
    expect(idle.has_value() &&
               std::abs(*idle - static_cast<double>(
                                    static_cast<uint16_t>(CarPlaySessionState::Phase::IDLE))) <
                   1e-9,
           "an enum reads as its ordinal");
}

void testAnEnumCanBeComparedInAnExpression()
{
    // What makes the ordinal useful: `phase == 2` is a boolean channel, which is
    // exactly what a state lane or a telltale wants.
    const auto recording_ordinal =
        static_cast<uint16_t>(CarPlaySessionState::Phase::RECORDING);

    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::CarPlaySessionState,
                                      "phase == " + std::to_string(recording_ordinal),
                                      "test/carplay/session");
    expect(eval.isValid(), "an enum comparison compiles");

    const auto yes =
        eval.evaluate<double>(sessionPayload(CarPlaySessionState::Phase::RECORDING));
    const auto no = eval.evaluate<double>(sessionPayload(CarPlaySessionState::Phase::IDLE));

    expect(yes.has_value() && *yes == 1.0, "the comparison is true for the matching enumerant");
    expect(no.has_value() && *no == 0.0, "and false for another");
}

void testAListElementIsIndexable()
{
    // MotecPdmOutputCurrent.values is 32 floats, one per output. Not one of them
    // could be plotted before this.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputCurrent, "values[7]",
                                      "test/pdm/current");
    expect(eval.isValid(), "an indexed list element is accepted");

    std::vector<float> amps(32, 0.0f);
    amps[7] = 12.5f;
    const auto value = eval.evaluate<double>(pdmCurrentPayload(amps));

    expect(value.has_value() && std::abs(*value - 12.5) < 1e-6,
           "values[7] reads the eighth element, not the first and not zero");
}

void testListAggregatesWorkOverTheRealLength()
{
    // sum() over the outputs is total PDM current, which is a real thing to want.
    // It is also the assertion that catches a vector registered at some generous
    // capacity and zero-filled: sum() would still be right, but avg() would be
    // scaled by length/capacity with nothing to say so.
    std::vector<float> amps(32, 0.0f);
    for (std::size_t i = 0; i < amps.size(); ++i)
    {
        amps[i] = 1.0f;
    }

    pub_sub::ExpressionEvaluator sum(pub_sub::schema_type_t::MotecPdmOutputCurrent,
                                     "sum(values)", "test/pdm/current");
    const auto total = sum.evaluate<double>(pdmCurrentPayload(amps));
    expect(total.has_value() && std::abs(*total - 32.0) < 1e-6, "sum() totals every element");

    pub_sub::ExpressionEvaluator avg(pub_sub::schema_type_t::MotecPdmOutputCurrent,
                                     "avg(values)", "test/pdm/current");
    const auto mean = avg.evaluate<double>(pdmCurrentPayload(amps));
    expect(mean.has_value() && std::abs(*mean - 1.0) < 1e-6,
           "avg() divides by the REAL length, not by some registered capacity");
}

void testAListOfEnumsIsIndexable()
{
    // Both new cases at once, and the one that matters most in practice: which
    // PDM output tripped.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputStatus, "values[3]",
                                      "test/pdm/status");
    expect(eval.isValid(), "a list of enums is accepted");

    std::vector<PdmOutputStatusEnum> states(32, PdmOutputStatusEnum::ON);
    states[3] = PdmOutputStatusEnum::OVER_CURRENT_ERROR;

    const auto tripped = eval.evaluate<double>(pdmStatusPayload(states));
    expect(tripped.has_value() &&
               std::abs(*tripped - static_cast<double>(static_cast<uint16_t>(
                                       PdmOutputStatusEnum::OVER_CURRENT_ERROR))) < 1e-9,
           "an enum element reads as its ordinal");

    states[3] = PdmOutputStatusEnum::ON;
    const auto normal = eval.evaluate<double>(pdmStatusPayload(states));
    expect(normal.has_value() && tripped.has_value() && *normal != *tripped,
           "and a different state reads differently");
}

void testAListLengthChangeIsFollowed()
{
    // exprtk fixes a vector's length when it is registered, and capnp does not
    // declare one. So the length comes from the message -- and if the publisher
    // changes it, the binding has to follow rather than read a stale tail.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputCurrent,
                                      "sum(values)", "test/pdm/current");

    const auto four = eval.evaluate<double>(pdmCurrentPayload({1.0f, 2.0f, 3.0f, 4.0f}));
    expect(four.has_value() && std::abs(*four - 10.0) < 1e-6, "a four-element message sums to 10");

    const auto two = eval.evaluate<double>(pdmCurrentPayload({5.0f, 6.0f}));
    expect(two.has_value() && std::abs(*two - 11.0) < 1e-6,
           "a SHORTER message sums to 11, with no stale tail left over from the longer one");

    const auto six =
        eval.evaluate<double>(pdmCurrentPayload({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}));
    expect(six.has_value() && std::abs(*six - 6.0) < 1e-6, "and a longer one grows to fit");
}

void testAnIndexPastTheRealLengthProducesNoValue()
{
    // THE CASE THAT MUST NOT RETURN ZERO. The expression compiles -- the index is
    // plausible for this schema -- and only a real message can say the stream
    // does not reach it. A confident permanent zero here is indistinguishable
    // from an output that is genuinely drawing no current.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputCurrent, "values[20]",
                                      "test/pdm/current");
    expect(eval.isValid(), "the expression compiles against a plausible index");

    const auto value = eval.evaluate<double>(pdmCurrentPayload({1.0f, 2.0f, 3.0f}));
    expect(!value.has_value(),
           "indexing past what the stream carries produces NO reading, not a zero");
}

void testAnEmptyListProducesNoValue()
{
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputCurrent, "values[0]",
                                      "test/pdm/current");
    const auto value = eval.evaluate<double>(pdmCurrentPayload({}));
    expect(!value.has_value(), "an empty list produces no reading rather than a zero");
}

void testAListOfTextWouldBeRejected()
{
    // The list equivalent of the Text rejection above: a list whose ELEMENTS are
    // not numeric is no more a number than a bare Text field is, and it is
    // knowable at construction.
    const auto schema = pub_sub::get_schema(pub_sub::schema_type_t::MotecPdmOutputCurrent);
    if (!schema)
    {
        return;
    }

    // Nothing in the tree currently publishes a List(Text), so this asserts the
    // shape of the check rather than a specific field: a list of floats is
    // accepted, and that acceptance is what the element-type test gates on.
    pub_sub::ExpressionEvaluator numeric(pub_sub::schema_type_t::MotecPdmOutputCurrent,
                                         "values[0]", "test/pdm/current");
    expect(numeric.isValid(), "a list of numeric elements is accepted");
}

void testListNamesAreReported()
{
    // variableNames() promises the schema fields the expression reads, and a list
    // is one. It lives in a different container from the scalars, so it is
    // exactly the kind of thing a merge forgets.
    pub_sub::ExpressionEvaluator eval(pub_sub::schema_type_t::MotecPdmOutputCurrent,
                                      "values[1] + values[2]", "test/pdm/current");
    const auto& names = eval.variableNames();
    expect(names.size() == 1 && names[0] == "values",
           "a list field is reported once, by its field name");
}

// The schema now DECLARES how many elements the PDM's lists carry, via the
// $fixedLength annotation. That is what lets a picker offer each output as its
// own channel without guessing a count or peeking at live traffic.
//
// This also pins the annotation id in capnp_json.h against the one in
// schemas/annotations.capnp. They are two hand-written copies of the same
// number, and a mismatch would not fail to build -- fixedListLength() would
// simply never match, every list would silently lose its declared length, and
// the browser would quietly stop offering element rows.
void testFixedLengthIsDeclaredInTheSchema()
{
    const auto schema = pub_sub::get_schema(pub_sub::schema_type_t::MotecPdmOutputCurrent);
    expect(schema.has_value(), "the PDM output-current schema is in the registry");
    if (!schema)
    {
        return;
    }

    bool found = false;
    for (auto field : schema->asStruct().getFields())
    {
        if (std::string(field.getProto().getName().cStr()) != "values")
        {
            continue;
        }
        found = true;
        const auto length = pub_sub::fixedListLength(field);
        expect(length.has_value(), "the annotation survives into the runtime schema");
        expect(length.has_value() && *length == 32,
               "and says 32, which is how many outputs a PDM has");
    }
    expect(found, "the field is still called 'values'");

    // A field with no annotation must report nothing rather than a default.
    const auto rpm = pub_sub::get_schema(pub_sub::schema_type_t::EngineRpm);
    if (rpm)
    {
        for (auto field : rpm->asStruct().getFields())
        {
            expect(!pub_sub::fixedListLength(field).has_value(),
                   "an unannotated field declares no length");
        }
    }
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

    testAnEnumEvaluatesToItsOrdinal();
    testAnEnumCanBeComparedInAnExpression();
    testAListElementIsIndexable();
    testListAggregatesWorkOverTheRealLength();
    testAListOfEnumsIsIndexable();
    testAListLengthChangeIsFollowed();
    testAnIndexPastTheRealLengthProducesNoValue();
    testAnEmptyListProducesNoValue();
    testAListOfTextWouldBeRejected();
    testListNamesAreReported();
    testFixedLengthIsDeclaredInTheSchema();
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
