// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control-session-message parameter codec.
//
// test_framing already drives whole messages through the link layer; this goes
// at the codec's edges, where the input is a byte string the phone chose and
// the wrong reading is silent. A parameter that decodes as absent looks exactly
// like a parameter the phone never sent, and the feature that depended on it
// simply never happens.
#include "iap2/csm.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
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

using Bytes = std::vector<uint8_t>;

// One parameter on the wire: big-endian total length, big-endian id, payload.
Bytes param(uint16_t id, const Bytes& payload)
{
    const uint16_t total = static_cast<uint16_t>(4 + payload.size());
    Bytes out{static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total),
              static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes concat(const std::vector<Bytes>& parts)
{
    Bytes out;
    for (const Bytes& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

}  // namespace

int main()
{
    using namespace iap2::csm;

    // The parameter parser, against input the phone controls.
    {
        const Bytes wire = concat({param(1, {0xAB}), param(2, {0xDE, 0xAD}), param(3, {})});
        const auto parsed = parseParams(wire.data(), wire.size());
        expect(parsed.has_value() && parsed->size() == 3, "three parameters parse");
        if (parsed)
        {
            expect((*parsed)[0].id == 1 && (*parsed)[0].data == Bytes({0xAB}), "first");
            expect((*parsed)[1].id == 2 && (*parsed)[1].data == Bytes({0xDE, 0xAD}), "second");
            expect((*parsed)[2].id == 3 && (*parsed)[2].data.empty(), "a zero-length parameter");
        }

        expect(parseParams(nullptr, 0).has_value() && parseParams(nullptr, 0)->empty(),
               "no parameters is a valid empty list, not an error");
    }

    // Malformed lengths must be rejected rather than trusted -- a declared
    // length is an offset into a buffer, so believing a wrong one reads past it.
    {
        // Declares 4 bytes of header but only 3 are present.
        const Bytes trailing{0x00, 0x05, 0x00};
        expect(!parseParams(trailing.data(), trailing.size()).has_value(),
               "a truncated parameter header is rejected");

        // Declares more than remains.
        const Bytes lying{0x00, 0x40, 0x00, 0x01, 0xFF};
        expect(!parseParams(lying.data(), lying.size()).has_value(),
               "a parameter claiming more bytes than remain is rejected");

        // Declares less than its own header, which would not advance the
        // offset and would spin forever.
        const Bytes tiny{0x00, 0x02, 0x00, 0x01};
        expect(!parseParams(tiny.data(), tiny.size()).has_value(),
               "a parameter shorter than its own header is rejected");

        // Exactly the header, no payload, is legal.
        const Bytes bare{0x00, 0x04, 0x00, 0x07};
        const auto parsed = parseParams(bare.data(), bare.size());
        expect(parsed.has_value() && parsed->size() == 1 && (*parsed)[0].data.empty(),
               "a header-only parameter is legal");
    }

    // Zero-length booleans. docs/carplay_bringup.md calls this out as a
    // hardware suspect: the iAP2 spec allows presence to mean true, and we
    // follow LIVI in treating it as absent instead. If a phone ever does send
    // one, CarPlayAvailability reads falsy and the session silently never
    // starts -- so the behaviour is pinned here rather than left to be
    // rediscovered.
    {
        ParamList params;
        params.push_back(Param{1, {}});
        params.push_back(Param{2, {0x01}});
        params.push_back(Param{3, {0x00}});

        expect(!getBool(params, 1).has_value(), "a zero-length boolean reads as ABSENT, not true");
        expect(getBool(params, 2) == true, "a 1 byte reads true");
        expect(getBool(params, 3) == false, "a 0 byte reads false");
        expect(!getBool(params, 99).has_value(), "a missing boolean is absent");

        // Any non-zero byte is true, not just 1.
        ParamList odd;
        odd.push_back(Param{1, {0x7F}});
        expect(getBool(odd, 1) == true, "any non-zero byte reads true");
    }

    // Integer widths. A width mismatch has to read as absent rather than
    // silently taking the wrong bytes.
    {
        ParamList params;
        params.push_back(Param{1, {0x12}});
        params.push_back(Param{2, {0x12, 0x34}});
        params.push_back(Param{3, {0x12, 0x34, 0x56, 0x78}});

        expect(getU8(params, 1) == 0x12, "u8");
        expect(getU16(params, 2) == 0x1234, "u16 is big endian");
        expect(getU32(params, 3) == 0x12345678u, "u32 is big endian");

        expect(!getU16(params, 1).has_value(), "a 1 byte parameter is not a u16");
        expect(!getU32(params, 2).has_value(), "a 2 byte parameter is not a u32");
        expect(!getU8(params, 2).has_value(), "a 2 byte parameter is not a u8");
    }

    // Strings. The protocol NUL-terminates; a value that does not must not
    // lose its last character.
    {
        ParamList params;
        params.push_back(Param{1, {'A', 'b', 'c', 0x00}});
        params.push_back(Param{2, {'X', 'y'}});
        params.push_back(Param{3, {0x00}});
        params.push_back(Param{4, {}});

        expect(getString(params, 1) == "Abc", "a NUL terminator is stripped");
        expect(getString(params, 2) == "Xy", "an unterminated string keeps every byte");
        expect(getString(params, 3) == "", "a lone NUL is the empty string");
        expect(getString(params, 4) == "", "a zero-length string is empty, not absent");
        expect(!getString(params, 9).has_value(), "a missing string is absent");
    }

    // Duplicate ids. The protocol permits them and find() takes the first,
    // which is what makes ParamList a list rather than a map.
    {
        ParamList params;
        params.push_back(Param{5, {0x01}});
        params.push_back(Param{5, {0x02}});
        expect(getU8(params, 5) == 0x01, "the first of a duplicated id wins");
        expect(has(params, 5), "has() finds it");
        expect(!has(params, 6), "and does not find what is absent");
    }

    // Nested groups: a parameter whose payload is itself a parameter list.
    {
        ParamList inner;
        addU16(inner, 1, 7000);
        addString(inner, 2, "nested");

        ParamList outer;
        addGroup(outer, 9, inner);
        addU8(outer, 10, 3);

        const Bytes encoded = encodeParams(outer);
        const auto parsed = parseParams(encoded.data(), encoded.size());
        expect(parsed.has_value(), "a message with a group round trips");
        if (parsed)
        {
            const auto group = getGroup(*parsed, 9);
            expect(group.has_value(), "the group parses");
            if (group)
            {
                expect(getU16(*group, 1) == 7000, "a value inside the group");
                expect(getString(*group, 2) == "nested", "and a string inside it");
            }
            expect(getU8(*parsed, 10) == 3, "the sibling parameter is unaffected");
            expect(!getGroup(*parsed, 10).has_value(),
                   "a non-group parameter does not parse as one");
        }
    }

    // Whole-message framing, including a length that disagrees with reality.
    {
        ParamList params;
        addU16(params, 1, 0x1234);
        const Bytes message = encodeMessage(0x4300, params);

        expect(message.size() >= 6, "a message has a header");
        expect(message[0] == 0x40 && message[1] == 0x40, "the start marker");
        expect(message[4] == 0x43 && message[5] == 0x00, "the message id");

        const auto parsed = parseMessage(message);
        expect(parsed.has_value() && parsed->id == 0x4300, "it parses back");
        expect(parsed && getU16(parsed->params, 1) == 0x1234, "with its parameter");

        // peekLength is how the stream reader finds a frame boundary.
        expect(peekLength(message.data(), message.size()) == message.size(),
               "peekLength reports the whole frame");
        expect(!peekLength(message.data(), 3).has_value(), "and waits for a whole header");

        Bytes short_declared = message;
        short_declared[2] = 0x00;
        short_declared[3] = 0x05;  // shorter than the header
        expect(!parseMessage(short_declared).has_value(),
               "a message declaring less than its header is rejected");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("csm tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
