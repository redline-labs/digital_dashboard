// SPDX-License-Identifier: GPL-3.0-or-later
//
// The encoding round trip: what a publisher stamps on a sample has to be
// something get_schema() can look up again.
//
// This exists because that round trip was broken for the entire life of the
// code and nothing noticed. Publishers set a MIME type and then attach the
// schema name via zenoh's Encoding::set_schema(), so what a subscriber reads
// back is the *combined* string "application/capnp;CanFrame" -- but the
// generated registry is keyed on the bare struct name. `inspect dump` fed the
// combined string straight to get_schema(), so the lookup could never match,
// for any key and any schema, and the tool silently fell back to a hex dump
// forever.
//
// No session and no network here: zenoh::Encoding is a standalone value type,
// so the test can drive the real encoder rather than a re-implementation of it.
// That matters -- a test that built the string itself would have agreed with
// the broken code.
#include "pub_sub/capnp_encoding.h"
#include "pub_sub/schema_registry.h"

#include "can_frame.capnp.h"

#include <zenoh.hxx>

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
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
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

template <typename T>
void expectEq(const T& actual, const T& expected, const std::string& what)
{
    ++checks;
    if (actual != expected)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {} (got '{}', expected '{}')", what, actual, expected);
    }
}

// Builds the encoding string exactly as ZenohPublisher, ZenohClient and
// ZenohService do -- same constant, same zenoh call, same serialisation.
std::string encodingStringFor(std::string_view schema_name)
{
    zenoh::Encoding encoding{pub_sub::kCapnpEncodingMime};
    encoding.set_schema(std::string(schema_name));
    return encoding.as_string();
}

// ---------------- the round trip, for every registered schema ----------------

// Iterating the registry rather than naming one schema means a schema added
// later is covered the day it is added, with no edit here.
void testEveryRegisteredSchemaRoundTrips()
{
    const auto schemas = pub_sub::get_available_schemas();
    expect(schemas.size() > 0, "the registry is not empty");

    for (const std::string_view name : schemas)
    {
        const std::string encoding = encodingStringFor(name);

        // 1. The publisher's string really does carry the schema after a ';'.
        //    If zenoh ever changed that separator this is what would catch it.
        expect(encoding.find(';') != std::string::npos,
               std::string("encoding for '") + std::string(name) + "' has a ';' separator");
        expect(encoding.rfind(std::string(pub_sub::kCapnpEncodingMime), 0) == 0,
               std::string("encoding for '") + std::string(name) + "' starts with the capnp MIME type");

        // 2. Parsing it back yields exactly the registry key.
        const std::string_view parsed = pub_sub::schemaNameFromEncoding(encoding);
        expectEq(std::string(parsed), std::string(name),
                 std::string("round trip recovers the schema name for '") + std::string(name) + "'");

        // 3. And that key actually resolves. This is the assertion that was
        //    false for every schema before the fix.
        expect(pub_sub::get_schema(parsed).has_value(),
               std::string("get_schema resolves '") + std::string(name) + "' after the round trip");
    }
}

// Pins the actual defect rather than only the fixed behaviour: feeding the raw
// encoding to get_schema must NOT resolve. If someone "simplifies" the parse
// away, this fails loudly instead of the tool quietly hex-dumping again.
void testRawEncodingDoesNotResolve()
{
    const std::string encoding = encodingStringFor(pub_sub::schema_traits<CanFrame>::name);
    expect(!pub_sub::get_schema(encoding).has_value(),
           "the unparsed encoding string is not a registry key (this was the bug)");

    expect(pub_sub::get_schema(pub_sub::schemaNameFromEncoding(encoding)).has_value(),
           "the parsed schema name is a registry key");
}

// ---------------- schemaNameFromEncoding in isolation ----------------

void testParseEdgeCases()
{
    expectEq(std::string(pub_sub::schemaNameFromEncoding("application/capnp;CanFrame")),
             std::string("CanFrame"), "splits MIME from schema");
    // No separator: hand back the whole thing. It will simply not match
    // anything, which is the right outcome for a non-capnp publisher.
    expectEq(std::string(pub_sub::schemaNameFromEncoding("text/plain")), std::string("text/plain"),
             "no separator yields the input unchanged");
    expectEq(std::string(pub_sub::schemaNameFromEncoding("application/capnp;")), std::string(""),
             "MIME type with an empty schema yields empty");
    expectEq(std::string(pub_sub::schemaNameFromEncoding("")), std::string(""),
             "empty encoding yields empty");
    // Only the first ';' separates; anything after belongs to the schema half.
    expectEq(std::string(pub_sub::schemaNameFromEncoding("a;b;c")), std::string("b;c"),
             "splits on the first separator only");

    // An empty or junk name must not resolve to something.
    expect(!pub_sub::get_schema("").has_value(), "empty name resolves to nothing");
    expect(!pub_sub::get_schema("NoSuchSchema").has_value(), "unknown name resolves to nothing");
}

// ---------------- the decode that the lookup exists to enable ----------------

// Finding the schema is only half of what `inspect dump` does; the other half
// is decoding the payload dynamically against it. Cover that too, so a schema
// that resolves but cannot actually read a message still fails here.
void testDynamicDecodeAgainstResolvedSchema()
{
    constexpr uint32_t kId = 1512;
    constexpr uint8_t kLen = 8;
    const std::vector<uint8_t> payload{4, 176, 6, 188, 7, 196, 1, 3};

    capnp::MallocMessageBuilder message;
    auto frame = message.initRoot<CanFrame>();
    frame.setId(kId);
    frame.setLen(kLen);
    auto data = frame.initData(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
    {
        data.set(i, payload[i]);
    }

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();

    // From here on, only what a subscriber has: the encoding and the bytes.
    const std::string encoding = encodingStringFor(pub_sub::schema_traits<CanFrame>::name);
    const auto schema = pub_sub::get_schema(pub_sub::schemaNameFromEncoding(encoding));
    if (!schema)
    {
        ++failures;
        ++checks;
        SPDLOG_ERROR("FAIL: cannot decode, schema did not resolve");
        return;
    }

    const auto flat = kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.begin()),
                                   bytes.size() / sizeof(capnp::word));
    capnp::FlatArrayMessageReader reader(flat);
    auto root = reader.getRoot<capnp::DynamicStruct>(schema->asStruct());

    expectEq(root.get("id").as<uint32_t>(), kId, "dynamic decode reads id");
    expectEq(root.get("len").as<uint8_t>(), kLen, "dynamic decode reads len");

    auto decoded = root.get("data").as<capnp::DynamicList>();
    expectEq(static_cast<size_t>(decoded.size()), payload.size(),
             "dynamic decode reads the full payload");
    for (size_t i = 0; i < payload.size() && i < decoded.size(); ++i)
    {
        expectEq(decoded[i].as<uint8_t>(), payload[i],
                 "dynamic decode reads payload byte " + std::to_string(i));
    }
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    testEveryRegisteredSchemaRoundTrips();
    testRawEncodingDoesNotResolve();
    testParseEdgeCases();
    testDynamicDecodeAgainstResolvedSchema();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} of {} assertion(s) failed", failures, checks);
        return 1;
    }
    SPDLOG_INFO("all {} capnp encoding assertions passed", checks);
    return 0;
}
