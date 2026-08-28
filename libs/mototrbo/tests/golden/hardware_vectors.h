// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bytes an XPR 5550 actually sent or accepted.
//
// Every vector here came off a real radio (serial 511TVMG951, firmware
// R02.10.00.0001) rather than being written from the same reading of the
// protocol as the parser -- which is the whole point, and the same argument
// gsof's golden records make. A vector authored from the documentation agrees
// with a parser authored from the documentation precisely where both are
// wrong, and this protocol was reverse-engineered: five defects in it were
// found only by putting the frames in front of the radio.
//
// The auth pairs are the strongest of these. A wrong TEA response is rejected
// by the radio with an all-zero CONN_REPLY, so a challenge/response pair from
// a live handshake proves the key and the cipher, not just that the code is
// self-consistent.

#ifndef MOTOTRBO_TEST_HARDWARE_VECTORS_H
#define MOTOTRBO_TEST_HARDWARE_VECTORS_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace mototrbo::golden
{

// Hex literal to bytes, at compile time, so the vectors below read as the hex
// a capture prints and still land in static_asserts.
template <std::size_t N>
constexpr std::array<std::uint8_t, (N - 1) / 2> hex(const char (&text)[N])
{
    static_assert((N - 1) % 2 == 0, "a hex literal needs an even number of digits");

    constexpr auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') { return static_cast<std::uint8_t>(c - '0'); }
        if (c >= 'a' && c <= 'f') { return static_cast<std::uint8_t>(c - 'a' + 10); }
        if (c >= 'A' && c <= 'F') { return static_cast<std::uint8_t>(c - 'A' + 10); }
        // Not a hex digit. In a constant expression this throws the evaluation
        // out and fails the build, which is the behaviour we want.
        throw "not a hex digit";
    };

    std::array<std::uint8_t, (N - 1) / 2> out {};
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    }

    return out;
}

// ---- XNL authentication ---------------------------------------------------
// Challenge/response pairs from live handshakes with the radio.
struct AuthPair
{
    std::array<std::uint8_t, 8> challenge;
    std::array<std::uint8_t, 8> response;
};

inline constexpr std::array<AuthPair, 2> kAuthPairs { {
    { hex("f7fe0c07fb7bac9f"), hex("3d868fc7798f6e95") },
    { hex("7fcffd1c79fa4dc0"), hex("4159bbadf565a66f") },
} };

// ---- 0x840D channel replies ----------------------------------------------
// Complete XCMP messages: opcode, result code, echoed operation, zone,
// channel. The echoed operation is the one that was ASKED FOR -- 0x80 only for
// a query -- which is why the up and down replies are here too.
inline constexpr auto kChannelQueryReply = hex("840d008000010002"); // zone 1, channel 2
inline constexpr auto kChannelUpReply    = hex("840d000300010003"); // zone 1, channel 3
inline constexpr auto kChannelDownReply  = hex("840d000400010002"); // zone 1, channel 2

// ---- 0xB4xx broadcasts ----------------------------------------------------
// Zone/channel, in both lengths the radio emits.
inline constexpr auto kChannelBroadcast4 = hex("b40d00010003");   // zone 1, channel 3
inline constexpr auto kChannelBroadcast5 = hex("b40d0001000200"); // zone 1, channel 2

// Display line 3, which reads "3 Talkaround" once the CSI escapes are removed.
// UTF-16 BIG-endian: note that codeplug strings are little-endian and this is
// not.
inline constexpr auto kDisplayLine3 = hex(
    "b40103010090001b005b0032003b00300048001b005b00310053001b005b0031003b"
    "00320035003b00320037003b00320034006d001b005b00300046001b005b0030003b"
    "003000300030003b003000300030003b003000300030004d001b005b0031003b0032"
    "00350035003b003200350035003b003200350035004d0033002000540061006c006b"
    "00610072006f0075006e00640000");

// Line 4 is the softkey row; its labels are separated by U+EFCD.
inline constexpr auto kDisplayLine4 = hex(
    "b4010401009e001b005b0033003b00300048001b005b00300053001b005b0030003b"
    "00320035003b00320037003b00320034006d001b005b00320046001b005b0030003b"
    "003200350035003b003200350035003b003200350035004d001b005b0031003b0030"
    "00300030003b003000300030003b003100300032004d004f0054002d0031efcd004f"
    "0054002d0032efcd004f0054002d0033efcd005a006e002d00730000");

// An event broadcast this build does not model. It must still parse -- as a
// named opcode with its bytes intact -- because a radio emitting something we
// have never seen should not look like a radio that has gone quiet.
inline constexpr auto kEventBroadcast = hex("b402010900000f00000000000000");

} // namespace mototrbo::golden

#endif // MOTOTRBO_TEST_HARDWARE_VECTORS_H
