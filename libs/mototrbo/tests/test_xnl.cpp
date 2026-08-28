// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XNL frame and the authentication that gets a session past the master.
//
// Most of this runs at COMPILE time. That is deliberate and it is the reason
// the library is written the way it is: every mistake this file guards against
// -- a swapped address pair, a length field counting the wrong bytes, a
// handshake payload two bytes short -- produces a frame a radio silently
// DROPS rather than one it complains about, so "the tests passed" and "the
// framing is right" are only the same statement when the assertions are
// checked against bytes a radio actually accepted.

#include "mototrbo/xnl.h"

#include "golden/hardware_vectors.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace mototrbo;

// ===========================================================================
// Compile time: authentication
// ===========================================================================

// Both pairs came off live handshakes. The radio rejects a wrong response with
// an all-zero CONN_REPLY, so these prove the key and the custom delta rather
// than merely proving this file agrees with xnl.cpp.
static_assert(xnl::auth_response(golden::kAuthPairs[0].challenge) == golden::kAuthPairs[0].response);
static_assert(xnl::auth_response(golden::kAuthPairs[1].challenge) == golden::kAuthPairs[1].response);

// Two synthetic vectors, computed from the algorithm as recovered. They are
// what catches a change to the cipher itself.
static_assert(xnl::auth_response(golden::hex("0102030405060708")) == golden::hex("f159e0084103c533"));
static_assert(xnl::auth_response(golden::hex("0000000000000000")) == golden::hex("bcd075e4014b88be"));

// The delta is NOT the textbook TEA constant. A build that "corrected" it
// would fail authentication with nothing in the log but a timeout.
static_assert(xnl::kTeaDelta != 0x9E3779B9u);

// ===========================================================================
// Compile time: the frame
// ===========================================================================

constexpr auto kSerializedDataMessage = [] {
    std::array<std::uint8_t, 32> buffer {};
    constexpr std::array<std::uint8_t, 4> payload { 0xDE, 0xAD, 0xBE, 0xEF };

    xnl::Frame frame;
    frame.opcode = xnl::Opcode::DataMessage;
    frame.protocol = 1;
    frame.flags = 3;
    frame.dst = 0x0100;
    frame.src = 0x000A;
    frame.transaction = 0x1234;
    frame.payload = payload;

    const std::size_t size = xnl::serialize_frame(frame, buffer).value_or(0);
    return std::pair<std::array<std::uint8_t, 32>, std::size_t> { buffer, size };
}();

constexpr std::span<const std::uint8_t> kDataMessageWire { kSerializedDataMessage.first.data(),
                                                           kSerializedDataMessage.second };

static_assert(kSerializedDataMessage.second == xnl::frame_size(4));

// The length field counts everything after itself: 12 header bytes plus the
// payload, NOT the two bytes it occupies.
static_assert(kDataMessageWire[0] == 0x00 && kDataMessageWire[1] == 12 + 4);

// dst at offset 6, src at offset 8. Reversing these is the mis-reading that
// stood in the first edition of this protocol description; it is confirmed
// three ways now.
static_assert(kDataMessageWire[6] == 0x01 && kDataMessageWire[7] == 0x00);
static_assert(kDataMessageWire[8] == 0x00 && kDataMessageWire[9] == 0x0A);

static_assert(xnl::parse_frame(kDataMessageWire)->opcode == xnl::Opcode::DataMessage);
static_assert(xnl::parse_frame(kDataMessageWire)->dst == 0x0100);
static_assert(xnl::parse_frame(kDataMessageWire)->src == 0x000A);
static_assert(xnl::parse_frame(kDataMessageWire)->transaction == 0x1234);
static_assert(xnl::parse_frame(kDataMessageWire)->protocol == 1);
static_assert(xnl::parse_frame(kDataMessageWire)->payload.size() == 4);
static_assert(xnl::parse_frame(kDataMessageWire)->payload[0] == 0xDE);

// frame_length answers from the first two bytes alone, which is what a stream
// reader has before it has the frame.
static_assert(xnl::frame_length(kDataMessageWire).value() == kSerializedDataMessage.second);

// ---- malformed input ------------------------------------------------------
// A short read is Truncated -- "come back with more bytes" -- and must stay
// distinguishable from a frame that is genuinely inconsistent.
static_assert(xnl::parse_frame(kDataMessageWire.first(kDataMessageWire.size() - 1)).error().kind ==
              ErrorKind::Truncated);
static_assert(xnl::parse_frame(std::span<const std::uint8_t> {}).error().kind == ErrorKind::Truncated);
static_assert(xnl::frame_length(std::span<const std::uint8_t> {}).error().kind == ErrorKind::Truncated);

// A length field smaller than the header it introduces.
static_assert(xnl::parse_frame(golden::hex("000500000000000000000000000000")).error().kind ==
              ErrorKind::LengthMismatch);

// A payload length that overflows the frame the length field declared: 12
// header bytes and a payload of 8, inside a frame that claims 12 total.
static_assert(xnl::parse_frame(golden::hex("000c000b010000000000000000080102030405060708")).error().kind ==
              ErrorKind::LengthMismatch);

// A buffer too small for the frame is refused rather than half-written.
static_assert(!xnl::serialize_frame(xnl::Frame {}, std::span<std::uint8_t> {}).has_value());

// ===========================================================================
// Compile time: the handshake payloads
// ===========================================================================

// TWELVE bytes, not ten. The obvious reading -- address then response -- is
// silently dropped by the radio, with no reply of any kind. This is the defect
// that blocked every live command until hardware found it.
static_assert(xnl::conn_request_payload(golden::kAuthPairs[0].challenge).size() == 12);
static_assert(xnl::conn_request_payload(golden::kAuthPairs[0].challenge)[2] == xnl::kDeviceType);
static_assert(xnl::conn_request_payload(golden::kAuthPairs[0].challenge)[4] == golden::kAuthPairs[0].response[0]);

// The assigned address is at CONN_REPLY+2, not +0. This radio does not
// validate our source address, so reading it from +0 is latent rather than
// fatal -- which is exactly why a capture never showed it.
static_assert(xnl::parse_conn_reply(golden::hex("0105000401010000000000000000")).value() == 0x0004);

// A rejected authentication comes back all zero, and a zero address is the
// only signal that it was rejected.
static_assert(xnl::parse_conn_reply(golden::hex("00000000")).error().kind == ErrorKind::AuthRejected);
static_assert(xnl::parse_conn_reply(golden::hex("0105")).error().kind == ErrorKind::Truncated);

static_assert(xnl::parse_auth_reply(golden::hex("000af7fe0c07fb7bac9f"))->temporaryAddress == 0x000A);
static_assert(xnl::parse_auth_reply(golden::hex("000af7fe0c07fb7bac9f"))->challenge ==
              golden::kAuthPairs[0].challenge);
static_assert(xnl::parse_auth_reply(golden::hex("000af7fe0c07fb7b")).error().kind == ErrorKind::Truncated);

// ===========================================================================
// Run time: the cipher is invertible, which the golden vectors alone do not
// show. A cipher that mapped everything to one value would pass those.
// ===========================================================================

void teaDecryptBlock(std::uint32_t& v0, std::uint32_t& v1)
{
    const auto& key = xnl::kTeaKey;
    std::uint32_t a = v0;
    std::uint32_t b = v1;
    std::uint32_t sum = xnl::kTeaDelta * 32u;

    for (int round = 0; round < 32; ++round)
    {
        b -= ((a << 4) + key[2]) ^ (a + sum) ^ ((a >> 5) + key[3]);
        a -= ((b << 4) + key[0]) ^ (b + sum) ^ ((b >> 5) + key[1]);
        sum -= xnl::kTeaDelta;
    }

    v0 = a;
    v1 = b;
}

void checkCipherRoundTrip()
{
    for (const std::uint32_t seed : { 0u, 1u, 0xDEADBEEFu, 0x12345678u })
    {
        std::uint32_t v0 = seed;
        std::uint32_t v1 = ~seed;

        std::uint32_t e0 = v0;
        std::uint32_t e1 = v1;
        xnl::tea_encrypt_block(e0, e1);
        check(e0 != v0 || e1 != v1, "TEA must actually change the block");

        teaDecryptBlock(e0, e1);
        check(e0 == v0 && e1 == v1, "TEA round trip");
    }
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    checkCipherRoundTrip();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    SPDLOG_INFO("mototrbo_test_xnl passed");
    return 0;
}
