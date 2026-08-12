// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Trimble "data collector format" (DCOL / TRIMCOMM) packet, which is the
// outer frame for everything the receiver says and everything it is told:
//
//     STX(0x02) | STATUS | TYPE | LENGTH | DATA[LENGTH] | CHECKSUM | ETX(0x03)
//
// LENGTH counts only DATA, so a whole packet is LENGTH + 6 bytes and can never
// exceed 261. CHECKSUM is (STATUS + TYPE + LENGTH + sum(DATA)) mod 256 -- every
// byte between STX and the checksum itself, and neither of the two frame
// markers.
//
// Everything here is constexpr, both directions. Parsing a packet at compile
// time is what lets the record tests assert against golden captures without
// running anything; building one at compile time is what lets a configuration
// command be a `constexpr auto` whose bytes are checked against the ICD table
// by static_assert rather than by a receiver refusing it on a bench.
//
// Reference: receiverhelp.trimble.com/oem-gnss/, "RS-232 serial interface
// specification" -> "Data collector format".
#ifndef GSOF_TRIMCOMM_H
#define GSOF_TRIMCOMM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gsof/error.h"

namespace gsof::trimcomm
{

inline constexpr std::uint8_t kStx = 0x02;
inline constexpr std::uint8_t kEtx = 0x03;

// STX + STATUS + TYPE + LENGTH ahead of DATA, CHECKSUM + ETX after it.
inline constexpr std::size_t kHeaderSize = 4;
inline constexpr std::size_t kFooterSize = 2;
inline constexpr std::size_t kOverheadSize = kHeaderSize + kFooterSize;

// LENGTH is one byte, so this is a hard ceiling rather than a chosen limit.
inline constexpr std::size_t kMaxDataSize = 255;
inline constexpr std::size_t kMaxPacketSize = kMaxDataSize + kOverheadSize;

// The packet types this tree uses. The wire byte is kept unconverted in
// PacketView, because a receiver is free to send a type we have never heard of
// and that must not be an error -- the frame is self-delimiting, so an unknown
// type is skippable.
enum class PacketType : std::uint8_t
{
    // Positive acknowledgement of a command. Carries no data.
    Ack = 0x06,
    // Negative acknowledgement: the receiver understood the framing and
    // refused the request.
    Nak = 0x15,
    // GENOUT. The GSOF report stream. Its DATA starts with the three-byte
    // transport header -- see transport.h.
    GenOut = 0x40,
    // GETOPT: ask which receiver options are installed.
    GetOpt = 0x4A,
    // RETOPT: the answer to GETOPT.
    RetOpt = 0x4B,
    // GETRAW / RAWDATA: position and real-time survey data.
    GetRaw = 0x56,
    RawData = 0x57,
    // APPFILE. Configuration, in both directions: sent as a command to change
    // the receiver's settings, and sent back by the receiver in response to
    // GETAPPFILE. Its DATA also starts with the three-byte transport header,
    // which is why one page assembler serves both this and GENOUT.
    AppFile = 0x64,
    // GETAPPFILE: ask for a stored application file by index.
    GetAppFile = 0x65,
    // RETAFDIR: the application file directory listing.
    RetAfDir = 0x67,
    // DELAPPFILE: delete a stored application file.
    DelAppFile = 0x68,
    // BREAKREQ: stop whatever streamed output is in progress.
    BreakReq = 0x6F,
};

constexpr const char* to_string(PacketType type)
{
    switch (type)
    {
        case PacketType::Ack:        return "ACK";
        case PacketType::Nak:        return "NAK";
        case PacketType::GenOut:     return "GENOUT";
        case PacketType::GetOpt:     return "GETOPT";
        case PacketType::RetOpt:     return "RETOPT";
        case PacketType::GetRaw:     return "GETRAW";
        case PacketType::RawData:    return "RAWDATA";
        case PacketType::AppFile:    return "APPFILE";
        case PacketType::GetAppFile: return "GETAPPFILE";
        case PacketType::RetAfDir:   return "RETAFDIR";
        case PacketType::DelAppFile: return "DELAPPFILE";
        case PacketType::BreakReq:   return "BREAKREQ";
    }

    // Not a default: -- the switch above must keep failing to compile when a
    // type is added to the enum. A receiver may legitimately send a type this
    // build has never heard of, and that is what lands here.
    return "unknown";
}

// A validated packet. `data` points into the buffer that was parsed, so it
// lives exactly as long as that buffer does.
struct PacketView
{
    // The receiver status byte. Kept raw: the ICD documents its bits per
    // receiver family and we have not verified them against a BD992, so
    // decoding it here would be a guess that looks like a fact.
    std::uint8_t status { 0 };

    // The wire byte, not the enum, so an unrecognised type round-trips.
    std::uint8_t type { 0 };

    std::span<const std::uint8_t> data;

    constexpr bool is(PacketType wanted) const
    {
        return type == static_cast<std::uint8_t>(wanted);
    }
};

// (STATUS + TYPE + LENGTH + sum(DATA)) mod 256. The accumulator is deliberately
// 8-bit: the sum is defined modulo 256 and letting it wrap is the definition,
// not an overflow.
constexpr std::uint8_t checksum(std::uint8_t status, std::uint8_t type, std::span<const std::uint8_t> data)
{
    std::uint8_t sum = static_cast<std::uint8_t>(status + type + static_cast<std::uint8_t>(data.size()));
    for (std::uint8_t byte : data)
    {
        sum = static_cast<std::uint8_t>(sum + byte);
    }
    return sum;
}

// How many bytes a packet whose LENGTH byte reads `length` occupies in total.
constexpr std::size_t packet_size(std::uint8_t length)
{
    return static_cast<std::size_t>(length) + kOverheadSize;
}

// Validate one packet at the front of `bytes`. Trailing bytes are ignored, so
// this can be pointed at a stream buffer.
//
// Truncated means "not enough bytes yet" and is the only error a stream reader
// should treat as routine; everything else means the framing is wrong and the
// reader has to resynchronise.
constexpr Result<PacketView> parse_packet(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kOverheadSize)
    {
        return truncated(0);
    }

    if (bytes[0] != kStx)
    {
        return bad_framing(0);
    }

    const std::uint8_t status = bytes[1];
    const std::uint8_t type = bytes[2];
    const std::uint8_t length = bytes[3];
    const std::size_t total = packet_size(length);

    if (bytes.size() < total)
    {
        return truncated(static_cast<std::uint16_t>(bytes.size()));
    }

    if (bytes[total - 1] != kEtx)
    {
        return bad_framing(static_cast<std::uint16_t>(total - 1));
    }

    const std::span<const std::uint8_t> data = bytes.subspan(kHeaderSize, length);

    if (bytes[total - 2] != checksum(status, type, data))
    {
        return bad_checksum(static_cast<std::uint16_t>(total - 2));
    }

    return PacketView { status, type, data };
}

// Build a packet around a fixed-size payload. The size is in the type, so the
// result is a std::array a caller can hold as `constexpr auto` -- which is the
// point: a command whose bytes are wrong then fails to compile against a
// static_assert instead of being refused by a receiver on a bench.
//
// STATUS is zero in a command; the ICD shows 00h in every command example and
// the receiver ignores it on input.
template <std::size_t N>
constexpr std::array<std::uint8_t, N + kOverheadSize> make_packet(
    PacketType type, const std::array<std::uint8_t, N>& data, std::uint8_t status = 0)
{
    static_assert(N <= kMaxDataSize, "a DCOL packet carries at most 255 data bytes");

    std::array<std::uint8_t, N + kOverheadSize> out {};
    out[0] = kStx;
    out[1] = status;
    out[2] = static_cast<std::uint8_t>(type);
    out[3] = static_cast<std::uint8_t>(N);

    for (std::size_t i = 0; i < N; ++i)
    {
        out[kHeaderSize + i] = data[i];
    }

    out[N + kHeaderSize] = checksum(status, static_cast<std::uint8_t>(type),
                                    std::span<const std::uint8_t>(data.data(), N));
    out[N + kHeaderSize + 1] = kEtx;

    return out;
}

// The runtime form, for a payload whose size is only known when the receiver's
// current configuration has been read back. Writes into `out` and returns how
// many bytes were used, or Truncated when `out` is too small -- the same shape
// as can::socketcan::encode_frame, for the same reason: the caller owns the
// buffer and the encoder never allocates.
constexpr Result<std::size_t> encode_packet(PacketType type, std::span<const std::uint8_t> data,
                                            std::span<std::uint8_t> out, std::uint8_t status = 0)
{
    if (data.size() > kMaxDataSize)
    {
        return too_long(static_cast<std::uint16_t>(data.size()));
    }

    const std::size_t total = data.size() + kOverheadSize;
    if (out.size() < total)
    {
        return truncated(static_cast<std::uint16_t>(out.size()));
    }

    out[0] = kStx;
    out[1] = status;
    out[2] = static_cast<std::uint8_t>(type);
    out[3] = static_cast<std::uint8_t>(data.size());

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        out[kHeaderSize + i] = data[i];
    }

    out[total - 2] = checksum(status, static_cast<std::uint8_t>(type), data);
    out[total - 1] = kEtx;

    return total;
}

} // namespace gsof::trimcomm

#endif // GSOF_TRIMCOMM_H
