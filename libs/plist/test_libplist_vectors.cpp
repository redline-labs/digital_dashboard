// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden vectors captured from libplist, frozen.
//
// These documents were produced by libplist 2.6.0 -- the implementation running
// on the other end of every usbmux and lockdown socket we talk to -- while it
// was still a vendored dependency. The differential test that generated them
// (plist_test_libplist_conformance) went away with the dependency; this keeps
// what it proved.
//
// The direction that matters is decode: a peer's document must read back as the
// value it encodes. Encoding is only pinned where we deliberately match
// libplist's layout byte for byte, which is the simple-dict case below --
// notably *not* <data>, where libplist wraps at 68 columns and we wrap at 60.
// Both parse fine either way; the difference is cosmetic and is asserted to be
// tolerated rather than eliminated.
//
// Regenerating these requires libplist, which is gone. Treat them as fixtures:
// if one needs to change, the change wants justifying against a real device.
#include "plist/binary.h"
#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

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

Bytes fromHex(const std::string& hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// --- kLibplistResultReply ---
const char* kLibplistResultReply =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>MessageType</key>\n"
    "\t<string>Result</string>\n"
    "\t<key>Number</key>\n"
    "\t<integer>0</integer>\n"
    "</dict>\n"
    "</plist>\n"
    "";

// --- kLibplistListDevicesReply ---
const char* kLibplistListDevicesReply =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>DeviceList</key>\n"
    "\t<array>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>DeviceID</key>\n"
    "\t\t\t<integer>1</integer>\n"
    "\t\t\t<key>MessageType</key>\n"
    "\t\t\t<string>Attached</string>\n"
    "\t\t\t<key>Properties</key>\n"
    "\t\t\t<dict>\n"
    "\t\t\t\t<key>ConnectionType</key>\n"
    "\t\t\t\t<string>USB</string>\n"
    "\t\t\t\t<key>SerialNumber</key>\n"
    "\t\t\t\t<string>00008140-000138EE0184801C</string>\n"
    "\t\t\t\t<key>DeviceID</key>\n"
    "\t\t\t\t<integer>1</integer>\n"
    "\t\t\t\t<key>ProductID</key>\n"
    "\t\t\t\t<integer>4776</integer>\n"
    "\t\t\t</dict>\n"
    "\t\t</dict>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n"
    "";

// --- kLibplistDataShapes ---
const char* kLibplistDataShapes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Blob</key>\n"
    "\t<data>\n"
    "\tAAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEy\n"
    "\tMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fYGFiY2Rl\n"
    "\tZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6PkJGSk5SVlpeY\n"
    "\tmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/wMHCw8TFxsfIycrL\n"
    "\tzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v8PHy8/T19vf4+fr7/P3+\n"
    "\t/w==\n"
    "\t</data>\n"
    "\t<key>Empty</key>\n"
    "\t<data>\n"
    "\t</data>\n"
    "\t<key>One</key>\n"
    "\t<data>\n"
    "\tQQ==\n"
    "\t</data>\n"
    "\t<key>Two</key>\n"
    "\t<data>\n"
    "\tQUI=\n"
    "\t</data>\n"
    "\t<key>Three</key>\n"
    "\t<data>\n"
    "\tQUJD\n"
    "\t</data>\n"
    "</dict>\n"
    "</plist>\n"
    "";

// --- kLibplistDataShapesBinary (368 bytes) ---
const char* kLibplistDataShapesBinary =
    "62706c6973743030d50103050709020406080a54426c6f624f11010000010203"
    "0405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223"
    "2425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40414243"
    "4445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60616263"
    "6465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80818283"
    "8485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3"
    "a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3"
    "c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3"
    "e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff55456d70"
    "747940534f6e6541415354776f42414255546872656543414243000800130018"
    "011c0122012301270129012d013001360000000000000201000000000000000b"
    "0000000000000000000000000000013a";

// --- kLibplistEscaping ---
const char* kLibplistEscaping =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>amp &amp; lt &lt; gt &gt;</key>\n"
    "\t<string>a &amp; b &lt; c &gt; d \" e ' f</string>\n"
    "\t<key>utf8</key>\n"
    "\t<string>café ☃ 🚗</string>\n"
    "\t<key>true</key>\n"
    "\t<true/>\n"
    "\t<key>false</key>\n"
    "\t<false/>\n"
    "\t<key>int64max</key>\n"
    "\t<integer>9223372036854775807</integer>\n"
    "\t<key>negative</key>\n"
    "\t<integer>-1</integer>\n"
    "</dict>\n"
    "</plist>\n"
    "";

// --- kLibplistEscapingBinary (181 bytes) ---
const char* kLibplistEscapingBinary =
    "62706c6973743030d601030507090b020406080a0c5f100f616d702026206c74"
    "203c206774203e5f10156120262062203c2063203e2064202220652027206654"
    "757466386900630061006600e9002026030020d83dde97547472756509556661"
    "6c73650858696e7436346d6178137fffffffffffffff586e6567617469766513"
    "ffffffffffffffff0815273f44575c5d63646d767f0000000000000101000000"
    "000000000d00000000000000000000000000000088";

// --- assertions --------------------------------------------------------------

// The usbmux Result reply, and the one case where our encoder is expected to
// reproduce libplist's layout exactly.
void testResultReply()
{
    const auto parsed = plist::decodeXml(kLibplistResultReply);
    expect(parsed.has_value(), "libplist Result reply parses");
    if (!parsed)
    {
        return;
    }
    const Value* type = parsed->find("MessageType");
    const Value* number = parsed->find("Number");
    expect(type != nullptr && type->asString() == "Result", "MessageType reads back");
    expect(number != nullptr && number->asInteger() == 0, "Number reads back");
    expect(parsed->keys() == std::vector<std::string>{"MessageType", "Number"},
           "key order is preserved");

    Value rebuilt = Value::dict();
    rebuilt.set("MessageType", Value::string("Result"));
    rebuilt.set("Number", Value::integer(0));
    expect(plist::encodeXml(rebuilt) == kLibplistResultReply,
           "our encoder reproduces libplist's layout byte for byte");
}

// The nested shape libusbmuxd actually receives from a mux.
void testListDevicesReply()
{
    const auto parsed = plist::decodeXml(kLibplistListDevicesReply);
    expect(parsed.has_value(), "libplist ListDevices reply parses");
    if (!parsed)
    {
        return;
    }
    const Value* list = parsed->find("DeviceList");
    expect(list != nullptr && list->isArray() && list->size() == 1, "DeviceList is a 1-element array");
    if (list == nullptr || list->size() != 1)
    {
        return;
    }
    const Value& entry = list->at(0);
    const Value* props = entry.find("Properties");
    expect(props != nullptr && props->isDict(), "Properties is a dict");
    if (props == nullptr)
    {
        return;
    }
    const Value* serial = props->find("SerialNumber");
    const Value* pid = props->find("ProductID");
    expect(serial != nullptr && serial->asString() == "00008140-000138EE0184801C",
           "the dashed serial reads back");
    // libplist writes 0x12a8 in decimal; the value has to survive that.
    expect(pid != nullptr && pid->asInteger() == 0x12a8, "ProductID reads back as 4776");
}

// The value both data vectors encode.
Value dataShapes()
{
    Bytes all(256);
    for (size_t i = 0; i < all.size(); ++i)
    {
        all[i] = static_cast<uint8_t>(i);
    }
    Value d = Value::dict();
    d.set("Blob", Value::data(all));
    d.set("Empty", Value::data({}));
    d.set("One", Value::data({'A'}));
    d.set("Two", Value::data({'A', 'B'}));
    d.set("Three", Value::data({'A', 'B', 'C'}));
    return d;
}

// <data> in both formats: base64, every byte value, all three padding cases, and
// libplist's own line wrapping which our decoder has to tolerate.
void testDataShapes()
{
    const Value expected = dataShapes();

    const auto xml = plist::decodeXml(kLibplistDataShapes);
    expect(xml.has_value(), "libplist <data> document parses");
    expect(xml.has_value() && *xml == expected, "every byte value survives libplist's base64");

    const auto bin = plist::decodeBinary(fromHex(kLibplistDataShapesBinary));
    expect(bin.has_value(), "libplist binary <data> document parses");
    expect(bin.has_value() && *bin == expected, "every byte value survives libplist's bplist");

    // Our own encoding of the same value must read back identically, even though
    // the bytes differ from libplist's (68-column wrapping vs our 60).
    const auto ours = plist::decodeXml(plist::encodeXml(expected));
    expect(ours.has_value() && *ours == expected, "our own <data> round trips");
    expect(std::string(kLibplistDataShapes) != plist::encodeXml(expected),
           "the layouts really do differ, so the test above is not vacuous");
}

Value escapingShapes()
{
    Value e = Value::dict();
    e.set("amp & lt < gt >", Value::string("a & b < c > d \" e ' f"));
    e.set("utf8", Value::string("caf\xC3\xA9 \xE2\x98\x83 \xF0\x9F\x9A\x97"));
    e.set("true", Value::boolean(true));
    e.set("false", Value::boolean(false));
    e.set("int64max", Value::integer(9223372036854775807LL));
    e.set("negative", Value::integer(-1));
    return e;
}

// Entity escaping in both keys and values, multi-byte UTF-8 (including a
// character outside the BMP, which bplist stores as a surrogate pair), booleans,
// and the signed 64-bit extremes.
void testEscaping()
{
    const Value expected = escapingShapes();

    const auto xml = plist::decodeXml(kLibplistEscaping);
    expect(xml.has_value(), "libplist escaping document parses");
    expect(xml.has_value() && *xml == expected, "escapes and UTF-8 survive libplist's XML");

    const auto bin = plist::decodeBinary(fromHex(kLibplistEscapingBinary));
    expect(bin.has_value(), "libplist binary escaping document parses");
    expect(bin.has_value() && *bin == expected, "escapes and UTF-8 survive libplist's bplist");

    // libplist leaves " and ' unescaped in element text, which is legal XML and
    // which our decoder therefore has to accept as-is.
    expect(std::string(kLibplistEscaping).find("d \" e ' f") != std::string::npos,
           "the fixture really does carry unescaped quotes");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testResultReply();
    testListDevicesReply();
    testDataShapes();
    testEscaping();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} libplist vector assertion(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all libplist golden vector tests passed");
    return 0;
}
