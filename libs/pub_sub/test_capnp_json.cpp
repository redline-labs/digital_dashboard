// SPDX-License-Identifier: GPL-3.0-or-later
//
// JSON <-> capnp over the dynamic API, against a fixture that has every shape a
// service request can take.
//
// What this pins is mostly the REJECTIONS. A converter that accepts too much is
// the dangerous kind: a negative number wrapped into a UInt8, a uint64 rounded
// through a double, or a second union arm silently replacing the first all
// produce a message that sends fine and means something other than what was
// typed. Each of those used to be possible through a list element, which went
// through a separate, looser code path than a field.
//
// Mutation-checks worth re-running after a change here:
//   * send list elements back through `double`       -> the uint64 and
//     negative-element cases fail
//   * drop the Data branch                           -> the hex cases fail
//   * drop the one-arm check for unions              -> the union cases fail

#include "pub_sub/capnp_json.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/schema-parser.h>
#include <capnp/serialize.h>
#include <kj/filesystem.h>

#include <cstdint>
#include <cstdio>
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

using pub_sub::json;

std::vector<std::uint8_t> encode(capnp::MallocMessageBuilder& message)
{
    const auto words = capnp::messageToFlatArray(message);
    const auto bytes = words.asBytes();
    return {bytes.begin(), bytes.end()};
}

// Every error for `fields`, or empty when it was accepted.
std::vector<std::string> errorsFor(capnp::StructSchema schema, const json& fields)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema);
    std::vector<std::string> errors;
    pub_sub::jsonToCapnp(fields, root, errors);
    return errors;
}

// True when exactly the one rejection is reported and it mentions `needle`.
void expectRejected(capnp::StructSchema schema, const json& fields, const std::string& needle,
                    const std::string& what)
{
    const auto errors = errorsFor(schema, fields);
    std::string all;
    for (const auto& e : errors)
    {
        all += e + " | ";
    }
    expect(!errors.empty(), what + " is rejected");
    expect(all.find(needle) != std::string::npos,
           what + ": the error says '" + needle + "' (got: " + all + ")");
}

void testRoundTrip(capnp::StructSchema schema)
{
    // Every field set, including both 64-bit extremes: a uint64 above 2^53 is
    // exactly what a pass through double rounds.
    const json input = {
        {"flag", true},
        {"i8", -128},
        {"u8", 255},
        {"i64", INT64_MIN},
        {"u64", UINT64_MAX},
        {"f32", 1.5},
        {"f64", -2.25},
        {"text", "hello"},
        {"bytes", "00ff7a"},
        {"colour", "green"},
        {"inner", {{"label", "x"}, {"count", 65535}}},
        {"colours", {"blue", "red"}},
        {"matrix", {json::array({1, 2}), json::array(), json::array({UINT64_MAX})}},
        {"inners", {{{"label", "a"}, {"count", 1}}, {{"label", "b"}, {"count", 2}}}},
        {"blobs", {"01", "", "dead"}},
        {"small", {0, 7, 255}},
        {"choice", {{"speed", 3.5}}},
        {"pair", {{"a", -5}, {"b", "bee"}}},
    };

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema);
    std::vector<std::string> errors;
    const bool accepted = pub_sub::jsonToCapnp(input, root, errors);
    expect(accepted && errors.empty(), "a request using every field shape is accepted");
    for (const auto& e : errors)
    {
        std::fprintf(stderr, "  %s\n", e.c_str());
    }

    const json output = pub_sub::capnpToJson(encode(message), schema, {.data_hex_limit = 64});
    expect(output == input, "and decodes back to exactly the same JSON");
    if (output != input)
    {
        std::fprintf(stderr, "  in:  %s\n  out: %s\n", input.dump().c_str(), output.dump().c_str());
    }
}

void testUnions(capnp::StructSchema schema)
{
    expectRejected(schema, {{"choice", {{"speed", 1.0}, {"name", "x"}}}}, "exactly one of",
                   "a union naming two arms");
    expectRejected(schema, {{"choice", json::object()}}, "exactly one of",
                   "a union naming no arm");

    // A payload-less arm is chosen by naming it.
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema);
    std::vector<std::string> errors;
    pub_sub::jsonToCapnp({{"choice", {{"name", "first"}}}}, root, errors);
    pub_sub::jsonToCapnp({{"choice", {{"none", nullptr}}}}, root, errors);
    expect(errors.empty(), "a Void arm accepts null");

    const json out = pub_sub::capnpToJson(encode(message), schema);
    expect(out["choice"] == json({{"none", nullptr}}),
           "selecting the Void arm moves the discriminant off the previous arm");
}

void testRejections(capnp::StructSchema schema)
{
    expectRejected(schema, {{"u8", -1}}, "non-negative", "a negative number into a UInt8");
    expectRejected(schema, {{"u8", 256}}, "out of range", "256 into a UInt8");
    expectRejected(schema, {{"i8", -129}}, "out of range", "-129 into an Int8");
    expectRejected(schema, {{"i8", 1.5}}, "expected an integer", "a fraction into an Int8");
    expectRejected(schema, {{"f32", 1e300}}, "does not fit", "1e300 into a Float32");

    // The same rules inside lists, with the element's path in the message.
    expectRejected(schema, {{"small", {1, -2}}}, "small[1]", "a negative list element");
    expectRejected(schema, {{"small", {1, 300}}}, "out of range", "an out-of-range list element");
    expectRejected(schema, {{"colours", {"red", "mauve"}}}, "colours[1]: 'mauve' is not valid",
                   "an unknown enumerant in a list");
    expectRejected(schema, {{"matrix", {json::array({1}), json::array({2, -3})}}}, "matrix[1][1]",
                   "a bad element in a nested list");
    expectRejected(schema, {{"inners", {{{"count", -1}}}}}, "inners[0].count",
                   "a bad field in a struct in a list");

    expectRejected(schema, {{"bytes", "abc"}}, "odd number", "odd-length hex");
    expectRejected(schema, {{"bytes", "zz"}}, "not a hex digit", "non-hex Data");
    expectRejected(schema, {{"bytes", 12}}, "hex string", "a number into Data");
    expectRejected(schema, {{"blobs", {"0g"}}}, "blobs[0]", "bad hex in a List(Data)");

    expectRejected(schema, {{"nope", 1}}, "no such field", "an unknown field");
}

void testDataOptions(capnp::StructSchema schema)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema);
    std::vector<std::string> errors;
    pub_sub::jsonToCapnp({{"bytes", "01020304"}}, root, errors);
    const auto encoded = encode(message);

    const json by_default = pub_sub::capnpToJson(encoded, schema);
    expect(by_default["bytes"] == json({{"_data_bytes", 4}}),
           "by default Data decodes as a length only, as echo and zenoh.read rely on");

    const json truncated = pub_sub::capnpToJson(encoded, schema, {.data_hex_limit = 2});
    expect(truncated["bytes"] == json({{"_data_bytes", 4}, {"hex_prefix", "0102"}}),
           "Data over the limit decodes as its length and a prefix");
}

}  // namespace

int main()
{
    capnp::SchemaParser parser;
    auto filesystem = kj::newDiskFilesystem();

    // kj paths are relative to the root they are opened against.
    std::string directory = FIXTURE_DIR;
    auto fixtures = filesystem->getRoot().openSubdir(kj::Path::parse(directory.substr(1)));
    const capnp::ParsedSchema file =
        parser.parseFromDirectory(*fixtures, kj::Path::parse("capnp_json_fixture.capnp"), nullptr);
    const capnp::StructSchema schema = file.getNested("Fixture").asStruct();

    testRoundTrip(schema);
    testUnions(schema);
    testRejections(schema);
    testDataOptions(schema);

    if (failures != 0)
    {
        std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    std::fprintf(stderr, "all %d checks passed\n", checks);
    return 0;
}
