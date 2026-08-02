// SPDX-License-Identifier: GPL-3.0-or-later
//
// NTB16 framing, tested on synthetic blocks.
//
// Every video frame and audio packet the phone sends crosses parseNtb(), and
// until this suite existed none of that code was reachable by any test on any
// platform: it was a private member of the Linux-only NcmBridge. So the cases
// below are weighted towards what a malformed or hostile block can do -- a
// backwards NDP chain used to be an infinite loop, and a datagram pointer that
// runs off the end used to be an out-of-bounds read.
//
// The round-trip tests pin the shape LIVI's ncm_bridge.py emits, which is what
// hardware has accepted.
#include "apple_usb/ncm_frame.h"

#include <spdlog/spdlog.h>

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
        SPDLOG_ERROR("FAIL: {} (got {}, expected {})", what, actual, expected);
    }
}

using Bytes = std::vector<uint8_t>;

void put16(Bytes& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(Bytes& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// A datagram to place in a block: its payload, and where the pointer table
// should claim it lives (defaulted to "wherever it really is").
struct Datagram
{
    Bytes payload;
    // Overrides for the pointer table, so a test can lie about index/length.
    int index_override = -1;
    int length_override = -1;
};

// Builds an NTB16 by hand rather than with buildNtb(), so parseNtb() is tested
// against an independent encoder and the two cannot agree on a shared mistake.
// One NDP holding every datagram, laid out after the pointer table.
Bytes makeNtb(const std::vector<Datagram>& datagrams, uint16_t sequence = 1,
              uint32_t nth_signature = apple_usb::kNth16Signature,
              uint32_t ndp_signature = apple_usb::kNdp16Signature)
{
    const size_t ndp_index = apple_usb::kNth16Length;
    // NDP16: 8-byte header + one 4-byte entry per datagram + 4-byte terminator.
    const size_t ndp_length = 8 + (datagrams.size() * 4) + 4;
    const size_t data_start = ndp_index + ndp_length;

    Bytes block;
    put32(block, nth_signature);
    put16(block, static_cast<uint16_t>(apple_usb::kNth16Length));
    put16(block, sequence);
    put16(block, 0);  // wBlockLength, patched below
    put16(block, static_cast<uint16_t>(ndp_index));

    put32(block, ndp_signature);
    put16(block, static_cast<uint16_t>(ndp_length));
    put16(block, 0);  // wNextNdpIndex: no chain

    size_t offset = data_start;
    for (const auto& d : datagrams)
    {
        const auto idx = d.index_override >= 0 ? static_cast<uint16_t>(d.index_override)
                                               : static_cast<uint16_t>(offset);
        const auto len = d.length_override >= 0 ? static_cast<uint16_t>(d.length_override)
                                                : static_cast<uint16_t>(d.payload.size());
        put16(block, idx);
        put16(block, len);
        offset += d.payload.size();
    }
    put16(block, 0);  // terminator index
    put16(block, 0);  // terminator length

    for (const auto& d : datagrams)
    {
        block.insert(block.end(), d.payload.begin(), d.payload.end());
    }

    block[8] = static_cast<uint8_t>(block.size());
    block[9] = static_cast<uint8_t>(block.size() >> 8);
    return block;
}

Bytes pattern(size_t len, uint8_t seed)
{
    Bytes out(len);
    for (size_t i = 0; i < len; ++i)
    {
        out[i] = static_cast<uint8_t>(seed + i);
    }
    return out;
}

// ---------------- parseNtb ----------------

void testParseSingleDatagram()
{
    const Bytes frame = pattern(64, 0x10);
    const auto frames = apple_usb::parseNtb(makeNtb({{frame}}));
    expectEq(frames.size(), size_t{1}, "single datagram yields one frame");
    if (frames.size() == 1)
    {
        expect(frames[0] == frame, "single datagram round-trips byte for byte");
    }
}

void testParseMultipleDatagrams()
{
    const Bytes a = pattern(60, 0x01);
    const Bytes b = pattern(128, 0x40);
    const Bytes c = pattern(1514, 0x80);  // a full-MTU ethernet frame
    const auto frames = apple_usb::parseNtb(makeNtb({{a}, {b}, {c}}));
    expectEq(frames.size(), size_t{3}, "three datagrams in one NDP yield three frames");
    if (frames.size() == 3)
    {
        expect(frames[0] == a, "datagram 0 intact");
        expect(frames[1] == b, "datagram 1 intact");
        expect(frames[2] == c, "datagram 2 (full MTU) intact");
    }
}

void testParseAcceptsCrcSignature()
{
    // "NCM1" -- the CRC variant. Only the low three bytes are significant.
    const Bytes frame = pattern(32, 0x55);
    const auto frames = apple_usb::parseNtb(
        makeNtb({{frame}}, 1, apple_usb::kNth16Signature, 0x314D434E));
    expectEq(frames.size(), size_t{1}, "NDP16 'NCM1' (CRC) signature is accepted");
}

void testParseRejectsBadNthSignature()
{
    const auto frames =
        apple_usb::parseNtb(makeNtb({{pattern(32, 0x11)}}, 1, 0xDEADBEEF));
    expectEq(frames.size(), size_t{0}, "bad NTH16 signature yields no frames");
}

void testParseRejectsBadNdpSignature()
{
    const auto frames = apple_usb::parseNtb(
        makeNtb({{pattern(32, 0x11)}}, 1, apple_usb::kNth16Signature, 0xDEADBEEF));
    expectEq(frames.size(), size_t{0}, "bad NDP16 signature yields no frames");
}

void testParseShortBlock()
{
    for (size_t len : {size_t{0}, size_t{1}, size_t{11}})
    {
        const Bytes truncated(len, 0);
        const auto frames = apple_usb::parseNtb(truncated);
        expectEq(frames.size(), size_t{0},
                 "block shorter than an NTH16 (" + std::to_string(len) + " bytes) yields nothing");
    }
}

void testParseTruncatedBlock()
{
    // A well-formed block cut in half: the pointer table still claims a
    // datagram that is no longer there. Must drop it, not read past the end.
    Bytes block = makeNtb({{pattern(200, 0x33)}});
    block.resize(block.size() / 2);
    const auto frames = apple_usb::parseNtb(block);
    expectEq(frames.size(), size_t{0}, "truncated block drops the datagram that ran off the end");
}

void testParseDatagramPointerOutOfRange()
{
    // Length lies: index is valid but index+len exceeds the block.
    const auto over = apple_usb::parseNtb(makeNtb({{pattern(64, 0x22), -1, 60000}}));
    expectEq(over.size(), size_t{0}, "datagram whose length runs off the block is dropped");

    // Index lies: points beyond the block entirely.
    const auto far = apple_usb::parseNtb(makeNtb({{pattern(64, 0x22), 60000, -1}}));
    expectEq(far.size(), size_t{0}, "datagram whose index is past the block is dropped");
}

void testParseKeepsGoodDatagramsAroundABadOne()
{
    // The contract is "yield what could be recovered", so a single bad pointer
    // must not discard its neighbours.
    const Bytes a = pattern(40, 0x01);
    const Bytes c = pattern(40, 0x03);
    const auto frames = apple_usb::parseNtb(makeNtb({{a}, {pattern(40, 0x02), -1, 60000}, {c}}));
    expectEq(frames.size(), size_t{2}, "a bad pointer drops only its own datagram");
    if (frames.size() == 2)
    {
        expect(frames[0] == a, "datagram before the bad one survives");
        expect(frames[1] == c, "datagram after the bad one survives");
    }
}

void testParseNdpIndexOutOfRange()
{
    Bytes block = makeNtb({{pattern(64, 0x44)}});
    // wNdpIndex at offset 10 -> past the end of the block.
    block[10] = 0xF0;
    block[11] = 0xFF;
    const auto frames = apple_usb::parseNtb(block);
    expectEq(frames.size(), size_t{0}, "NDP index past the block yields no frames");
}

void testParseBackwardsNdpChainTerminates()
{
    // wNextNdpIndex pointing at or before the current NDP is the infinite-loop
    // case. Reaching the assertion at all is the result being checked.
    Bytes block = makeNtb({{pattern(64, 0x66)}});
    const size_t ndp = apple_usb::kNth16Length;
    block[ndp + 6] = static_cast<uint8_t>(ndp);  // wNextNdpIndex = itself
    block[ndp + 7] = 0;
    const auto frames = apple_usb::parseNtb(block);
    expectEq(frames.size(), size_t{1}, "self-referential NDP chain stops after one pass");

    block[ndp + 6] = 4;  // wNextNdpIndex = before the first NDP
    block[ndp + 7] = 0;
    const auto backwards = apple_usb::parseNtb(block);
    expectEq(backwards.size(), size_t{1}, "backwards NDP chain stops after one pass");
}

void testParseNdpLengthTooSmall()
{
    Bytes block = makeNtb({{pattern(64, 0x77)}});
    const size_t ndp = apple_usb::kNth16Length;
    block[ndp + 4] = 8;  // wLength below the 12-byte minimum
    block[ndp + 5] = 0;
    const auto frames = apple_usb::parseNtb(block);
    expectEq(frames.size(), size_t{0}, "NDP16 with wLength < 12 yields no frames");
}

// ---------------- buildNtb ----------------

void testBuildRoundTrip()
{
    for (size_t len : {size_t{1}, size_t{60}, size_t{64}, size_t{512}, size_t{1514}})
    {
        const Bytes frame = pattern(len, 0x90);
        uint16_t seq = 0;
        const Bytes block = apple_usb::buildNtb(frame.data(), frame.size(), seq, 32764);
        const auto frames = apple_usb::parseNtb(block);
        expectEq(frames.size(), size_t{1},
                 "buildNtb(" + std::to_string(len) + ") parses back to one frame");
        if (frames.size() == 1)
        {
            expect(frames[0] == frame,
                   "buildNtb(" + std::to_string(len) + ") round-trips byte for byte");
        }
    }
}

void testBuildHeaderShape()
{
    const Bytes frame = pattern(100, 0xA0);
    uint16_t seq = 0;
    const Bytes block = apple_usb::buildNtb(frame.data(), frame.size(), seq, 32764);

    expectEq(block.size(), apple_usb::kTxDatagramOffset + frame.size(),
             "block is 28 bytes of framing plus the frame");
    // wNdpIndex points at the NDP, which directly follows the NTH.
    expectEq(static_cast<size_t>(block[10] | (block[11] << 8)), apple_usb::kNth16Length,
             "wNdpIndex points just past the NTH16");
    // The datagram pointer names offset 28.
    const size_t entry = apple_usb::kNth16Length + 8;
    expectEq(static_cast<size_t>(block[entry] | (block[entry + 1] << 8)),
             apple_usb::kTxDatagramOffset, "datagram index is 28");
    expectEq(static_cast<size_t>(block[entry + 2] | (block[entry + 3] << 8)), frame.size(),
             "datagram length matches the frame");
    // Terminator entry.
    expectEq(static_cast<size_t>(block[entry + 4] | (block[entry + 5] << 8)), size_t{0},
             "pointer table is (0,0) terminated");
}

void testBuildSequenceAdvances()
{
    const Bytes frame = pattern(48, 0xB0);
    uint16_t seq = 0;
    for (uint16_t expected = 1; expected <= 4; ++expected)
    {
        const Bytes block = apple_usb::buildNtb(frame.data(), frame.size(), seq, 32764);
        expectEq(seq, expected, "wSequence advances on every block");
        expectEq(static_cast<uint16_t>(block[6] | (block[7] << 8)), expected,
                 "wSequence in the header matches the counter");
    }
}

void testBuildSequenceWraps()
{
    const Bytes frame = pattern(48, 0xC0);
    uint16_t seq = 65535;
    apple_usb::buildNtb(frame.data(), frame.size(), seq, 32764);
    expectEq(seq, static_cast<uint16_t>(0), "wSequence wraps to 0 rather than overflowing");
}

void testBuildAvoidsBulkMultiple()
{
    // A block that is an exact multiple of 512 would need a zero-length packet
    // to terminate the bulk transfer; buildNtb pads by one byte instead.
    // 512 - 28 = 484.
    const Bytes frame = pattern(484, 0xD0);
    uint16_t seq = 0;
    const Bytes block = apple_usb::buildNtb(frame.data(), frame.size(), seq, 32764);
    expectEq(block.size(), size_t{513}, "a 512-byte block is padded to 513");
    expect(block.size() % 512 != 0, "emitted block is never a multiple of 512");

    // The pad must not corrupt the frame.
    const auto frames = apple_usb::parseNtb(block);
    expectEq(frames.size(), size_t{1}, "padded block still parses");
    if (frames.size() == 1)
    {
        expect(frames[0] == frame, "padding does not disturb the datagram");
    }
}

// ---------------- deriveEui64LinkLocal ----------------

void testEui64()
{
    // Universal/local bit of the first octet flips (0x02 ^ 0x02 = 0x00), and
    // ff:fe goes in the middle.
    expectEq(apple_usb::deriveEui64LinkLocal("02:00:00:00:00:01"),
             std::string("fe80::0:ff:fe00:1"), "EUI-64 flips the U/L bit and inserts ff:fe");
    expectEq(apple_usb::deriveEui64LinkLocal("00:1a:2b:3c:4d:5e"),
             std::string("fe80::21a:2bff:fe3c:4d5e"), "EUI-64 from a locally administered MAC");
    // Uppercase hex is valid in a MAC string.
    expectEq(apple_usb::deriveEui64LinkLocal("00:1A:2B:3C:4D:5E"),
             std::string("fe80::21a:2bff:fe3c:4d5e"), "EUI-64 accepts uppercase hex");
}

void testEui64Rejects()
{
    const char* bad[] = {
        "",                     // empty
        "02:00:00:00:00",       // five octets
        "02:00:00:00:00:01:02", // seven octets
        "02:00:00:00:00:01 ",   // trailing junk
        "02-00-00-00-00-01",    // wrong separator
        "0g:00:00:00:00:01",    // not hex
        "2:0:0:0:0:1",          // unpadded octets
    };
    for (const char* mac : bad)
    {
        expect(apple_usb::deriveEui64LinkLocal(mac).empty(),
               std::string("rejects malformed MAC '") + mac + "'");
    }
}

}  // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    testParseSingleDatagram();
    testParseMultipleDatagrams();
    testParseAcceptsCrcSignature();
    testParseRejectsBadNthSignature();
    testParseRejectsBadNdpSignature();
    testParseShortBlock();
    testParseTruncatedBlock();
    testParseDatagramPointerOutOfRange();
    testParseKeepsGoodDatagramsAroundABadOne();
    testParseNdpIndexOutOfRange();
    testParseBackwardsNdpChainTerminates();
    testParseNdpLengthTooSmall();

    testBuildRoundTrip();
    testBuildHeaderShape();
    testBuildSequenceAdvances();
    testBuildSequenceWraps();
    testBuildAvoidsBulkMultiple();

    testEui64();
    testEui64Rejects();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} of {} assertion(s) failed", failures, checks);
        return 1;
    }
    SPDLOG_INFO("all {} NTB16 framing assertions passed", checks);
    return 0;
}
