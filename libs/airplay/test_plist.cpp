// SPDX-License-Identifier: GPL-3.0-or-later
// Binary property list round-trip tests, plus decode/encode known answers taken
// from Apple's own plutil(1) so the wire format is pinned to the real thing.
#include "airplay/plist.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <string>

namespace
{

using airplay::plist::Bytes;
using airplay::plist::Value;

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

Bytes fromHex(std::string_view hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        out.push_back(static_cast<uint8_t>((hexDigit(hex[i]) << 4) | hexDigit(hex[i + 1])));
    }
    return out;
}

std::string toHex(const Bytes& bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes)
    {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

struct QuietLogs
{
    QuietLogs() { spdlog::set_level(spdlog::level::off); }
    ~QuietLogs() { spdlog::set_level(spdlog::level::info); }
};

bool roundTrips(const Value& value, const char* what)
{
    const Bytes encoded = airplay::plist::encode(value);
    if (encoded.empty())
    {
        SPDLOG_ERROR("FAIL: {} (encode returned nothing)", what);
        ++failures;
        return false;
    }
    const auto decoded = airplay::plist::decode(encoded);
    if (!decoded)
    {
        SPDLOG_ERROR("FAIL: {} (decode failed over {} bytes)", what, encoded.size());
        ++failures;
        return false;
    }
    if (!(*decoded == value))
    {
        SPDLOG_ERROR("FAIL: {} (value changed across the round trip)", what);
        ++failures;
        return false;
    }
    return true;
}

void testScalars()
{
    roundTrips(Value::boolean(true), "bool true round trip");
    roundTrips(Value::boolean(false), "bool false round trip");
    roundTrips(Value::integer(0), "integer 0 round trip");
    roundTrips(Value::integer(255), "integer 255 (1 byte) round trip");
    roundTrips(Value::integer(256), "integer 256 (2 byte) round trip");
    roundTrips(Value::integer(65536), "integer 65536 (4 byte) round trip");
    roundTrips(Value::integer(4294967296LL), "integer 2^32 (8 byte) round trip");
    roundTrips(Value::integer(130367356919LL), "integer AirPlay features round trip");
    roundTrips(Value::integer(-1), "integer -1 round trip");
    roundTrips(Value::integer(-42), "integer -42 round trip");
    roundTrips(Value::integer(INT64_MIN), "integer INT64_MIN round trip");
    roundTrips(Value::integer(INT64_MAX), "integer INT64_MAX round trip");
    roundTrips(Value::real(0.25), "real 0.25 round trip");
    roundTrips(Value::real(-1.5e300), "real -1.5e300 round trip");
    roundTrips(Value::string(""), "empty string round trip");
    roundTrips(Value::string("hello"), "ascii string round trip");
    roundTrips(Value::string(std::string(1000, 'x')), "long ascii string round trip");
    roundTrips(Value::string("Mercedes 190E \xe2\x80\x94 Motorhead"), "utf-8 string round trip");
    roundTrips(Value::string("emoji \xf0\x9f\x9a\x97 surrogate pair"), "non-BMP string round trip");
    roundTrips(Value::data({}), "empty data round trip");
    roundTrips(Value::data(fromHex("000102030405060708090a0b0c0d0e0f")), "data round trip");
    roundTrips(Value::data(Bytes(70000, 0xab)), "large data round trip (4-byte count)");
    roundTrips(Value::date(0.0), "date epoch round trip");
    roundTrips(Value::date(742000000.5), "date round trip");

    // Round-tripped scalars must keep their type, not just their bits.
    const auto decoded = airplay::plist::decode(airplay::plist::encode(Value::date(1.0)));
    expect(decoded && decoded->isDate() && decoded->asDate() == 1.0, "date keeps its type");
    const auto integer = airplay::plist::decode(airplay::plist::encode(Value::integer(7)));
    expect(integer && integer->isInteger() && integer->asInteger() == 7, "integer keeps its type");
    const auto real = airplay::plist::decode(airplay::plist::encode(Value::real(7.0)));
    expect(real && real->isReal() && real->asReal() == 7.0, "real keeps its type");
}

void testContainers()
{
    Value inner_array = Value::array();
    inner_array.push(Value::integer(1));
    inner_array.push(Value::string("two"));
    inner_array.push(Value::boolean(false));
    inner_array.push(Value::data(fromHex("cafebabe")));
    roundTrips(inner_array, "mixed array round trip");
    roundTrips(Value::array(), "empty array round trip");
    roundTrips(Value::dict(), "empty dict round trip");

    Value display = Value::dict();
    display.set("width", Value::integer(800));
    display.set("height", Value::integer(480));
    display.set("primaryInputDevice", Value::integer(1));
    display.set("features", Value::integer(14));

    Value displays = Value::array();
    displays.push(display);

    Value stream = Value::dict();
    stream.set("type", Value::integer(110));
    stream.set("timestampInfo", Value::array());
    stream.set("latencyMs", Value::real(12.5));

    Value streams = Value::array();
    streams.push(stream);

    Value root = Value::dict();
    root.set("deviceid", Value::string("AA:BB:CC:DD:EE:FF"));
    root.set("features", Value::integer(130367356919LL));
    root.set("statusFlags", Value::integer(68));
    root.set("protovers", Value::string("1.0"));
    root.set("sourceVersion", Value::string("220.68"));
    root.set("pk", Value::data(Bytes(32, 0x5a)));
    root.set("displays", displays);
    root.set("streams", streams);
    root.set("keepAliveLowPower", Value::boolean(true));
    root.set("nested", inner_array);
    root.set("sentinel", Value::date(1.0));
    expect(roundTrips(root, "GET /info shaped payload round trip"), "GET /info payload");

    // Deep nesting stresses the reference table widths.
    Value deep = Value::string("bottom");
    for (int i = 0; i < 20; ++i)
    {
        Value wrapper = Value::dict();
        wrapper.set("level", Value::integer(i));
        wrapper.set("child", deep);
        deep = wrapper;
    }
    roundTrips(deep, "20-level nesting round trip");

    // Enough objects to force a 2-byte reference size.
    Value wide = Value::array();
    for (int i = 0; i < 400; ++i)
    {
        Value entry = Value::dict();
        entry.set("i", Value::integer(i));
        entry.set("s", Value::string("entry-" + std::to_string(i)));
        wide.push(entry);
    }
    expect(roundTrips(wide, "400-element array round trip (2-byte refs)"), "wide array");

    // Accessors.
    const auto decoded = airplay::plist::decode(airplay::plist::encode(root));
    expect(decoded.has_value(), "decoded root");
    if (decoded)
    {
        expect(decoded->isDict() && decoded->size() == root.size(), "dict size preserved");
        expect(decoded->keys() == root.keys(), "dict key order preserved");
        const Value* displays_value = decoded->find("displays");
        expect(displays_value != nullptr && displays_value->isArray() && displays_value->size() == 1,
               "nested array survives");
        if (displays_value)
        {
            const Value* width = displays_value->at(0).find("width");
            expect(width != nullptr && width->asInteger() == 800, "nested dict value survives");
        }
        expect(decoded->contains("pk") && decoded->find("pk")->asData().size() == 32, "data value survives");
        expect(decoded->find("missing") == nullptr, "find returns null for a missing key");
        expect(decoded->at(99).isNull(), "out-of-range index yields null");
    }

    // set() replaces in place and keeps the original position.
    Value replace = Value::dict();
    replace.set("a", Value::integer(1));
    replace.set("b", Value::integer(2));
    replace.set("a", Value::integer(3));
    expect(replace.size() == 2 && replace.keys()[0] == "a" && replace.find("a")->asInteger() == 3,
           "dict set replaces in place");
}

void testAppleReference()
{
    // Produced by: plutil -convert binary1, macOS 15. A dict with an ASCII
    // string, 8-byte positive and negative integers, a 2-byte integer, a
    // double, a 16-byte data blob, a nested array of dicts and a UTF-16 string.
    const Bytes reference = fromHex(
        "62706c6973743030db0102030405060708090a0b0c0d0e0f1011121a1b1c1d58"
        "6e656761746976655970726f746f766572735b737461747573466c6167735864"
        "65766963656964576c6174656e637952706b58646973706c61797357756e6963"
        "6f64655866656174757265735a626f6f6c5f66616c736559626f6f6c5f747275"
        "6513ffffffffffffffd653312e3010445f101141413a42423a43433a44443a45"
        "453a4646233fd00000000000004f1010000102030405060708090a0b0c0d0e0f"
        "a113d31415161718195577696474685f10127072696d617279496e7075744465"
        "766963655668656967687411032010011101e06f101b004d0065007200630065"
        "00640065007300200031003900300045002020140020004d006f007400f60072"
        "006800650061006400202713130000001e5a7ffff708090008001f0028003200"
        "3e0047004f0052005b0063006c00770081008a008e009000a400ad00c000c200"
        "c900cf00e400eb00ee00f000f3012c0135013600000000000002010000000000"
        "00001e00000000000000000000000000000137");

    const auto decoded = airplay::plist::decode(reference);
    expect(decoded.has_value(), "Apple reference plist decodes");
    if (!decoded)
    {
        return;
    }

    expect(decoded->isDict() && decoded->size() == 11, "Apple reference is an 11-entry dict");
    expect(decoded->find("deviceid") != nullptr && decoded->find("deviceid")->asString() == "AA:BB:CC:DD:EE:FF",
           "Apple reference ASCII string");
    expect(decoded->find("features") != nullptr && decoded->find("features")->asInteger() == 130367356919LL,
           "Apple reference 8-byte integer");
    expect(decoded->find("statusFlags") != nullptr && decoded->find("statusFlags")->asInteger() == 68,
           "Apple reference small integer");
    expect(decoded->find("negative") != nullptr && decoded->find("negative")->asInteger() == -42,
           "Apple reference negative integer is sign-extended");
    expect(decoded->find("bool_true") != nullptr && decoded->find("bool_true")->isBool() &&
               decoded->find("bool_true")->asBool(),
           "Apple reference true");
    expect(decoded->find("bool_false") != nullptr && decoded->find("bool_false")->isBool() &&
               !decoded->find("bool_false")->asBool(true),
           "Apple reference false");
    expect(decoded->find("latency") != nullptr && decoded->find("latency")->isReal() &&
               decoded->find("latency")->asReal() == 0.25,
           "Apple reference real");
    expect(decoded->find("pk") != nullptr &&
               toHex(decoded->find("pk")->asData()) == "000102030405060708090a0b0c0d0e0f",
           "Apple reference data");
    expect(decoded->find("unicode") != nullptr &&
               decoded->find("unicode")->asString() == "Mercedes 190E \xe2\x80\x94 Mot\xc3\xb6rhead \xe2\x9c\x93",
           "Apple reference UTF-16 string decodes to UTF-8");

    const Value* displays = decoded->find("displays");
    expect(displays != nullptr && displays->isArray() && displays->size() == 1, "Apple reference nested array");
    if (displays && displays->size() == 1)
    {
        const Value& display = displays->at(0);
        expect(display.isDict() && display.size() == 3, "Apple reference nested dict");
        expect(display.find("width") != nullptr && display.find("width")->asInteger() == 800,
               "Apple reference nested integer (width)");
        expect(display.find("height") != nullptr && display.find("height")->asInteger() == 480,
               "Apple reference nested integer (height)");
        expect(display.find("primaryInputDevice") != nullptr &&
                   display.find("primaryInputDevice")->asInteger() == 1,
               "Apple reference nested integer (primaryInputDevice)");
    }

    // What we decoded must survive our own encoder.
    roundTrips(*decoded, "Apple reference re-encodes and decodes identically");
}

void testEncoderKnownAnswers()
{
    // Byte-exact comparisons against plutil output. Only shapes where Apple's
    // encoder makes the same choices we do (no key sorting, no deduplication)
    // can be compared this way.
    expect(toHex(airplay::plist::encode(Value::string("hello"))) ==
               "62706c6973743030"  // bplist00
               "5568656c6c6f"      // "hello"
               "08"                // offset table
               "000000000000010100000000000000010000000000000000000000000000000e",
           "encoder KAT: root string matches plutil");

    Value pair = Value::array();
    pair.push(Value::integer(1));
    pair.push(Value::integer(258));
    expect(toHex(airplay::plist::encode(pair)) ==
               "62706c6973743030"
               "a20102"  // array of 2, refs 1 and 2
               "1001"    // 1
               "110102"  // 258, as a 2-byte integer
               "080b0d"  // offset table
               "0000000000000101000000000000000300000000000000000000000000000010",
           "encoder KAT: array of two integers matches plutil");

    Value dict = Value::dict();
    dict.set("alpha", Value::string("beta"));
    expect(toHex(airplay::plist::encode(dict)) ==
               "62706c6973743030"
               "d10102"        // dict of 1, key ref 1, value ref 2
               "55616c706861"  // "alpha"
               "5462657461"    // "beta"
               "080b11"        // offset table
               "0000000000000101000000000000000300000000000000000000000000000016",
           "encoder KAT: single-entry dict matches plutil");
}

void testMalformed()
{
    const QuietLogs quiet;

    expect(!airplay::plist::decode({}).has_value(), "empty buffer is rejected");
    expect(!airplay::plist::decode(fromHex("00")).has_value(), "runt buffer is rejected");
    expect(!airplay::plist::decode(Bytes(64, 0x00)).has_value(), "bad magic is rejected");

    const Bytes good = airplay::plist::encode(Value::string("hello"));
    expect(airplay::plist::decode(good).has_value(), "sanity: the good buffer decodes");

    Bytes truncated(good.begin(), good.end() - 1);
    expect(!airplay::plist::decode(truncated).has_value(), "truncated trailer is rejected");

    // Object count that cannot fit in the buffer.
    Bytes bad_count = good;
    bad_count[bad_count.size() - 32 + 15] = 0xff;
    expect(!airplay::plist::decode(bad_count).has_value(), "impossible object count is rejected");

    // Top object index beyond the table.
    Bytes bad_top = good;
    bad_top[bad_top.size() - 32 + 23] = 0x7f;
    expect(!airplay::plist::decode(bad_top).has_value(), "out-of-range top object is rejected");

    // Offset table pointing outside the buffer.
    Bytes bad_table = good;
    bad_table[bad_table.size() - 32 + 31] = 0xff;
    expect(!airplay::plist::decode(bad_table).has_value(), "offset table past the end is rejected");

    // A string whose length runs past the end of the file.
    Bytes overlong = airplay::plist::encode(Value::string("hello"));
    overlong[8] = 0x5f;  // extended-count ASCII string, but no count follows
    expect(!airplay::plist::decode(overlong).has_value(), "overlong string is rejected");

    // Unsupported object type (0xe).
    Bytes bad_type = good;
    bad_type[8] = 0xe0;
    expect(!airplay::plist::decode(bad_type).has_value(), "unsupported object type is rejected");

    // A dict whose key is not a string.
    Bytes bad_key = fromHex(
        "62706c6973743030d10102"  // dict with 1 entry, refs 1 and 2
        "1001"                    // object 1: integer 1 (an illegal key)
        "5462657461"              // object 2: "beta"
        "080b0d"                  // offset table
        "0000000000000101000000000000000300000000000000000000000000000012");
    expect(!airplay::plist::decode(bad_key).has_value(), "non-string dict key is rejected");
}

// The decoder eats whatever the phone sends, so mutate a valid document and
// confirm nothing crashes, hangs or reads out of bounds. Deterministic seed so
// a failure is reproducible; worth running under ASan when this file changes.
void testMutationFuzz()
{
    const QuietLogs quiet;

    Value root = Value::dict();
    root.set("deviceid", Value::string("AA:BB:CC:DD:EE:FF"));
    root.set("features", Value::integer(130367356919LL));
    root.set("latency", Value::real(0.25));
    root.set("pk", Value::data(Bytes(32, 0x5a)));
    root.set("flag", Value::boolean(true));
    Value nested = Value::array();
    nested.push(Value::string("Motorhead \xc3\xb6"));
    nested.push(Value::date(1.0));
    root.set("nested", nested);

    const Bytes original = airplay::plist::encode(root);
    uint32_t state = 0x1234abcd;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    size_t survived = 0;
    for (size_t i = 0; i < 20000; ++i)
    {
        Bytes mutated = original;
        const size_t edits = 1 + (next() % 4);
        for (size_t e = 0; e < edits; ++e)
        {
            mutated[next() % mutated.size()] = static_cast<uint8_t>(next() & 0xff);
        }
        if (next() % 4 == 0)
        {
            mutated.resize(1 + (next() % mutated.size()));
        }
        // Only the absence of a crash matters; either outcome is legal.
        survived += airplay::plist::decode(mutated).has_value() ? 1 : 0;
    }
    expect(survived <= 20000, "mutation fuzz survived without crashing");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testScalars();
    testContainers();
    testAppleReference();
    testEncoderKnownAnswers();
    testMalformed();
    testMutationFuzz();

    if (failures == 0)
    {
        SPDLOG_INFO("plist tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} plist test(s) failed", failures);
    return EXIT_FAILURE;
}
