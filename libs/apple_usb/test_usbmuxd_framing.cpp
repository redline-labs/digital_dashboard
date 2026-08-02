// SPDX-License-Identifier: GPL-3.0-or-later
//
// The usbmuxd control protocol's framing.
//
// This is the one place in the stack where an unprivileged local client fully
// controls a number that becomes an allocation size. Anything that can reach
// the socket sends it -- our own node, libimobiledevice tools, whatever else a
// developer has running -- so the interesting cases are the dishonest ones: a
// length below the header, which underflows the body size to something near
// SIZE_MAX, and a length far above any real message.
//
// Also here: the UDID spelling conversion, which cost a hardware session
// (stage 4). A pair record filed under one spelling is invisible under the
// other, and the symptom is the phone re-prompting for trust on every connect.
#include "apple_usb/usbmuxd_framing.h"

#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// A header as a client would put it on the wire: four little-endian uint32s.
std::vector<uint8_t> header(uint32_t length, uint32_t version, uint32_t message, uint32_t tag)
{
    std::vector<uint8_t> out(16);
    const uint32_t fields[4] = {length, version, message, tag};
    std::memcpy(out.data(), fields, sizeof(fields));
    return out;
}

}  // namespace

int main()
{
    using namespace apple_usb::usbmuxd;

    // A well-formed header.
    {
        const std::vector<uint8_t> wire = header(16 + 42, kPlistVersion, kPlistMessage, 0xDEADBEEF);
        const auto parsed = parseHeader(wire.data());
        expect(parsed.has_value(), "a well-formed header parses");
        if (parsed)
        {
            expect(parsed->length == 58, "length");
            expect(parsed->version == kPlistVersion, "version");
            expect(parsed->message == kPlistMessage, "message type");
            expect(parsed->tag == 0xDEADBEEF, "tag, which the reply must echo");
            expect(parsed->bodySize() == 42, "body size is length minus the header");
        }
    }

    // The header is little-endian. Reading it big-endian turns a 58-byte
    // message into a 972-million-byte one, which is exactly the allocation the
    // bound exists to stop -- so the byte order is worth stating.
    {
        const std::vector<uint8_t> wire = header(0x11223344, kPlistVersion, kPlistMessage, 0);
        expect(wire[0] == 0x44 && wire[1] == 0x33 && wire[2] == 0x22 && wire[3] == 0x11,
               "the fixture writes little-endian, matching the protocol");
    }

    // A length below the header underflows bodySize() to near SIZE_MAX. This is
    // the case that matters most: believed, it is an immediate out-of-memory.
    {
        for (uint32_t length : {0u, 1u, 15u})
        {
            const std::vector<uint8_t> wire = header(length, kPlistVersion, kPlistMessage, 0);
            expect(!parseHeader(wire.data()).has_value(),
                   "a length of " + std::to_string(length) + " is rejected before it underflows");
        }

        // Exactly the header is legal and means an empty body.
        const std::vector<uint8_t> wire = header(16, kPlistVersion, kPlistMessage, 7);
        const auto parsed = parseHeader(wire.data());
        expect(parsed.has_value() && parsed->bodySize() == 0,
               "a header-only message is legal and has no body");
    }

    // And a length above the ceiling.
    {
        const std::vector<uint8_t> at = header(kMaxRequestBytes, kPlistVersion, kPlistMessage, 0);
        expect(parseHeader(at.data()).has_value(), "a message at the ceiling is accepted");

        for (uint32_t length : {kMaxRequestBytes + 1, 0x80000000u, 0xFFFFFFFFu})
        {
            const std::vector<uint8_t> wire = header(length, kPlistVersion, kPlistMessage, 0);
            expect(!parseHeader(wire.data()).has_value(),
                   "a length of " + std::to_string(length) + " is rejected");
        }
    }

    // Replies: the header the client parses back, and the body it reads.
    {
        const plist::Value dict = resultDict(0);
        const std::vector<uint8_t> wire = encodeReply(0x01020304, dict);

        expect(wire.size() > kHeaderSize, "a reply carries a body");
        const auto parsed = parseHeader(wire.data());
        expect(parsed.has_value(), "a reply's own header parses");
        if (parsed)
        {
            expect(parsed->length == wire.size(), "the declared length is the whole message");
            expect(parsed->bodySize() == wire.size() - kHeaderSize, "and the body is the rest");
            expect(parsed->version == kPlistVersion && parsed->message == kPlistMessage,
                   "a reply declares the plist version and message type");
            expect(parsed->tag == 0x01020304, "and echoes the request's tag");
        }

        // The body is XML the client can decode.
        const std::string_view body(reinterpret_cast<const char*>(wire.data() + kHeaderSize),
                                    wire.size() - kHeaderSize);
        const auto decoded = plist::decodeXml(body);
        expect(decoded.has_value() && decoded->isDict(), "the body is a decodable plist");
        if (decoded)
        {
            expect(dictString(*decoded, "MessageType") == "Result", "MessageType");
            const plist::Value* number = decoded->find("Number");
            expect(number != nullptr && number->asInteger() == 0, "Number");
        }
    }

    // dictString is how every request field is read, so absence and the wrong
    // type both have to be quiet rather than throwing.
    {
        plist::Value dict = plist::Value::dict();
        dict.set("Name", plist::Value::string("value"));
        dict.set("Count", plist::Value::integer(3));

        expect(dictString(dict, "Name") == "value", "a string value is returned");
        expect(dictString(dict, "Count").empty(), "a non-string value reads as empty");
        expect(dictString(dict, "Absent").empty(), "a missing key reads as empty");
        expect(dictString(plist::Value::dict(), "Anything").empty(), "an empty dict is fine");
    }

    // The UDID spelling. libusbmuxd and lockdown disagree about the dash in a
    // 24-character serial, and a pair record filed under one form cannot be
    // found under the other -- the phone then re-prompts for trust every time.
    {
        const std::string undashed = "001234567890ABCDEF123456";  // 24 chars
        const std::string dashed = "00123456-7890ABCDEF123456";   // 25, dash at index 8

        expect(undashed.size() == 24 && dashed.size() == 25, "the fixtures are the right lengths");
        expect(alternateUdidForm(undashed) == dashed, "undashed converts to dashed");
        expect(alternateUdidForm(dashed) == undashed, "dashed converts back");
        expect(alternateUdidForm(alternateUdidForm(undashed)) == undashed,
               "converting twice is the identity");

        // Anything that is neither form has no alternate, and must not be
        // guessed at -- a wrong guess files the record under a third spelling.
        expect(alternateUdidForm("").empty(), "empty has no alternate");
        expect(alternateUdidForm("short").empty(), "a short serial has no alternate");
        expect(alternateUdidForm(std::string(40, 'A')).empty(), "an over-long serial has none");
        expect(alternateUdidForm(std::string(24, '-')).empty(),
               "a 24-character string containing dashes is not the undashed form");
        expect(alternateUdidForm("0123456789-BCDEF123456789").empty(),
               "a 25-character string with the dash elsewhere is not the dashed form");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("usbmuxd framing tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
