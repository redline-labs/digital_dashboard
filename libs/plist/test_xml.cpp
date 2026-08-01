// SPDX-License-Identifier: GPL-3.0-or-later
// XML property list round-trip and known-answer tests.
//
// The known answers matter more than the round trips here: this codec exists to
// talk to libplist-based peers (libusbmuxd, lockdownd), so "we can read what we
// wrote" proves nothing on its own. The literal documents below were produced by
// libplist and by Apple's plutil(1).
#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace
{

using plist::Bytes;
using plist::Value;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

Value roundTrip(const Value& in, const std::string& what)
{
    const std::string xml = plist::encodeXml(in);
    const auto out = plist::decodeXml(xml);
    if (!out)
    {
        expect(false, what + ": did not decode");
        return Value();
    }
    expect(*out == in, what + ": round trip differs");
    return *out;
}

void testScalarRoundTrips()
{
    roundTrip(Value::string("hello"), "plain string");
    roundTrip(Value::string(""), "empty string");
    roundTrip(Value::boolean(true), "true");
    roundTrip(Value::boolean(false), "false");
    roundTrip(Value::integer(0), "zero");
    roundTrip(Value::integer(-1), "negative one");
    roundTrip(Value::integer(9223372036854775807LL), "int64 max");
    roundTrip(Value::integer(-9223372036854775807LL - 1), "int64 min");
    roundTrip(Value::real(0.5), "real");
    roundTrip(Value::real(-1.25e10), "large negative real");
    roundTrip(Value::data({}), "empty data");
    roundTrip(Value::data({0x00, 0x01, 0xFE, 0xFF}), "small data");

    // Every byte value, long enough to exercise the line wrapping.
    Bytes all(256);
    for (size_t i = 0; i < all.size(); ++i)
    {
        all[i] = static_cast<uint8_t>(i);
    }
    roundTrip(Value::data(all), "256-byte data");

    // Each of the three base64 padding cases.
    roundTrip(Value::data({0x41}), "data length 1 (two pad chars)");
    roundTrip(Value::data({0x41, 0x42}), "data length 2 (one pad char)");
    roundTrip(Value::data({0x41, 0x42, 0x43}), "data length 3 (no padding)");
}

void testEscaping()
{
    // The characters that must be escaped, and the ones that must not be
    // double-escaped on the way back.
    roundTrip(Value::string("a & b < c > d"), "escapable characters");
    roundTrip(Value::string("\"quoted\" and 'apostrophed'"), "quotes survive");
    roundTrip(Value::string("&amp;"), "a literal entity spelling round trips");
    roundTrip(Value::string("caf\xC3\xA9 \xE2\x98\x83"), "utf-8 passes through");

    Value dict = Value::dict();
    dict.set("key & <with> markup", Value::string("v"));
    roundTrip(dict, "markup in a key");

    // Numeric character references are a thing real writers emit.
    const auto decoded = plist::decodeXml(
        "<plist version=\"1.0\"><string>&#65;&#x42;&#67;</string></plist>");
    expect(decoded.has_value() && decoded->asString() == "ABC",
           "numeric character references decode");

    const auto utf8_ref =
        plist::decodeXml("<plist version=\"1.0\"><string>&#233;</string></plist>");
    expect(utf8_ref.has_value() && utf8_ref->asString() == "\xC3\xA9",
           "a non-ASCII character reference becomes UTF-8");
}

void testContainers()
{
    roundTrip(Value::dict(), "empty dict");
    roundTrip(Value::array(), "empty array");

    Value nested = Value::dict();
    nested.set("string", Value::string("s"));
    nested.set("int", Value::integer(42));
    nested.set("bool", Value::boolean(true));
    nested.set("data", Value::data({1, 2, 3}));

    Value inner = Value::array();
    inner.push(Value::integer(1));
    inner.push(Value::string("two"));
    inner.push(Value::dict());
    nested.set("array", inner);

    Value outer = Value::dict();
    outer.set("nested", nested);
    outer.set("empty_array", Value::array());
    roundTrip(outer, "nested containers");

    // Key order is part of the model and must survive.
    const auto out = plist::decodeXml(plist::encodeXml(nested));
    expect(out.has_value(), "nested dict decodes");
    if (out)
    {
        const std::vector<std::string> expected = {"string", "int", "bool", "data", "array"};
        expect(out->keys() == expected, "dict key order is preserved");
    }
}

// A dict two levels deep, exactly as libplist writes it -- tab indentation,
// DOCTYPE, trailing newline. Our encoder is expected to match byte for byte.
void testEncoderMatchesLibplistLayout()
{
    Value root = Value::dict();
    root.set("MessageType", Value::string("Result"));
    root.set("Number", Value::integer(0));

    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>MessageType</key>\n"
        "\t<string>Result</string>\n"
        "\t<key>Number</key>\n"
        "\t<integer>0</integer>\n"
        "</dict>\n"
        "</plist>\n";

    const std::string actual = plist::encodeXml(root);
    expect(actual == expected, "encoder layout matches libplist");
    if (actual != expected)
    {
        SPDLOG_ERROR("expected:\n{}\nactual:\n{}", expected, actual);
    }
}

// Documents captured from libusbmuxd on the wire. These are the exact requests
// our UsbmuxdServer has to parse.
void testRealUsbmuxdRequests()
{
    const std::string listen =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>BundleID</key>\n"
        "\t<string>org.libimobiledevice.libusbmuxd</string>\n"
        "\t<key>ClientVersionString</key>\n"
        "\t<string>usbmuxd built for freedom</string>\n"
        "\t<key>MessageType</key>\n"
        "\t<string>Listen</string>\n"
        "\t<key>ProgName</key>\n"
        "\t<string>libusbmuxd</string>\n"
        "\t<key>kLibUSBMuxVersion</key>\n"
        "\t<integer>3</integer>\n"
        "</dict>\n"
        "</plist>\n";

    const auto parsed = plist::decodeXml(listen);
    expect(parsed.has_value(), "libusbmuxd Listen parses");
    if (parsed)
    {
        const Value* mt = parsed->find("MessageType");
        expect(mt != nullptr && mt->asString() == "Listen", "MessageType reads back");
        const Value* version = parsed->find("kLibUSBMuxVersion");
        expect(version != nullptr && version->asInteger() == 3, "integer reads back");
        expect(parsed->find("Nonexistent") == nullptr, "missing key is absent");
    }

    // Connect carries the port as an integer in network byte order.
    const std::string connect =
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Connect</string>"
        "<key>DeviceID</key><integer>1</integer>"
        "<key>PortNumber</key><integer>32498</integer>"
        "</dict></plist>";
    const auto conn = plist::decodeXml(connect);
    expect(conn.has_value(), "Connect parses without whitespace");
    if (conn)
    {
        const Value* port = conn->find("PortNumber");
        expect(port != nullptr && port->asInteger() == 32498, "PortNumber reads back");
    }
}

// Pair records are dicts of PEM blobs; the <data> handling is what matters.
void testPairRecordShape()
{
    Value record = Value::dict();
    const std::string pem =
        "-----BEGIN CERTIFICATE-----\n"
        "MIICujCCAaKgAwIBAgIBADANBgkqhkiG9w0BAQsFADAAMB4XDTI2MDcyNzA0NTEw\n"
        "-----END CERTIFICATE-----\n";
    record.set("DeviceCertificate", Value::data(Bytes(pem.begin(), pem.end())));
    record.set("HostID", Value::string("C1FDE0B2-6F84-4B0E-8B4C-000000000000"));
    record.set("SystemBUID", Value::string("30142B2C-1234-4C63-9C7E-000000000000"));
    record.set("EscrowBag", Value::data(Bytes(32, 0xAB)));

    const Value out = roundTrip(record, "pair record");
    const Value* cert = out.find("DeviceCertificate");
    expect(cert != nullptr && cert->isData(), "certificate stays data");
    if (cert != nullptr)
    {
        const Bytes& bytes = cert->asData();
        expect(std::string(bytes.begin(), bytes.end()) == pem, "PEM survives base64");
    }
}

void testTolerance()
{
    // No XML declaration, no DOCTYPE, minified.
    expect(plist::decodeXml("<plist version=\"1.0\"><string>x</string></plist>").has_value(),
           "bare document parses");

    // Comments and processing instructions between elements.
    const auto commented = plist::decodeXml(
        "<?xml version=\"1.0\"?><!-- a note --><!DOCTYPE plist PUBLIC \"x\" \"y\">"
        "<plist version=\"1.0\"><!-- another --><dict>"
        "<key>k</key><!-- mid --><string>v</string></dict></plist>");
    expect(commented.has_value(), "comments are skipped");
    if (commented)
    {
        const Value* v = commented->find("k");
        expect(v != nullptr && v->asString() == "v", "value after a comment reads back");
    }

    // A DOCTYPE with an internal subset -- the '>' inside must not end it early.
    expect(plist::decodeXml("<!DOCTYPE plist [ <!ENTITY x \"y\"> ]>"
                            "<plist version=\"1.0\"><string>ok</string></plist>")
               .has_value(),
           "DOCTYPE internal subset is skipped");

    // Self-closing containers.
    const auto self_closed =
        plist::decodeXml("<plist version=\"1.0\"><dict/></plist>");
    expect(self_closed.has_value() && self_closed->isDict() && self_closed->size() == 0,
           "<dict/> is an empty dict");
    const auto empty_array = plist::decodeXml("<plist version=\"1.0\"><array/></plist>");
    expect(empty_array.has_value() && empty_array->isArray() && empty_array->size() == 0,
           "<array/> is an empty array");

    // <data> split across lines with arbitrary indentation.
    const auto wrapped = plist::decodeXml(
        "<plist version=\"1.0\"><data>\n\t\tQUJD\n\t\t</data></plist>");
    expect(wrapped.has_value(), "wrapped data parses");
    if (wrapped)
    {
        const Bytes& b = wrapped->asData();
        expect(std::string(b.begin(), b.end()) == "ABC", "wrapped data decodes");
    }
}

void testRejections()
{
    expect(!plist::decodeXml("").has_value(), "empty input is rejected");
    expect(!plist::decodeXml("not xml at all").has_value(), "garbage is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\">").has_value(),
           "unterminated plist is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><dict><key>k</key></dict></plist>")
                .has_value(),
           "a key with no value is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><dict><key>k</key><string>v</string></plist>")
                .has_value(),
           "an unclosed dict is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><banana>x</banana></plist>").has_value(),
           "an unknown element is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><integer>notanumber</integer></plist>")
                .has_value(),
           "a non-numeric integer is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><integer></integer></plist>").has_value(),
           "an empty integer is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><data>not*base64</data></plist>").has_value(),
           "invalid base64 is rejected");
    expect(!plist::decodeXml("<plist version=\"1.0\"><date>yesterday</date></plist>").has_value(),
           "an unparseable date is rejected");

    // Deep nesting must fail cleanly rather than blow the stack.
    std::string deep = "<plist version=\"1.0\">";
    for (int i = 0; i < 500; ++i)
    {
        deep += "<array>";
    }
    for (int i = 0; i < 500; ++i)
    {
        deep += "</array>";
    }
    deep += "</plist>";
    expect(!plist::decodeXml(deep).has_value(), "excessive nesting is rejected");
}

void testDates()
{
    // The Apple epoch itself, and a date either side of it.
    const auto epoch = plist::decodeXml(
        "<plist version=\"1.0\"><date>2001-01-01T00:00:00Z</date></plist>");
    expect(epoch.has_value() && std::fabs(epoch->asDate()) < 0.5,
           "the Apple epoch is zero");

    const auto later = plist::decodeXml(
        "<plist version=\"1.0\"><date>2026-07-31T19:00:00Z</date></plist>");
    expect(later.has_value() && later->asDate() > 0.0, "a later date is positive");
    if (later)
    {
        // Re-encoding must reproduce the same instant.
        const auto again = plist::decodeXml(plist::encodeXml(*later));
        expect(again.has_value() && std::fabs(again->asDate() - later->asDate()) < 0.5,
               "date round trips through the encoder");
    }

    const auto before = plist::decodeXml(
        "<plist version=\"1.0\"><date>1970-01-01T00:00:00Z</date></plist>");
    expect(before.has_value() && before->asDate() < 0.0, "a pre-epoch date is negative");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testScalarRoundTrips();
    testEscaping();
    testContainers();
    testEncoderMatchesLibplistLayout();
    testRealUsbmuxdRequests();
    testPairRecordShape();
    testTolerance();
    testRejections();
    testDates();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} XML plist assertion(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all XML plist tests passed");
    return 0;
}
