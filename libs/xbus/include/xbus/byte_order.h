// SPDX-License-Identifier: GPL-3.0-or-later
//
// Big-endian scalar reads and writes over a span of bytes, plus the two
// fixed-point formats the XBus protocol invents.
//
// The rationale for assembling from individual bytes rather than memcpy-ing a
// packed struct is the same one gsof/byte_order.h gives at length: memcpy is
// not usable in constant evaluation, a byteswap-on-read parser is silently
// wrong on a big-endian host, and packed members are unaligned-access UB. This
// file is not a copy of that one -- sharing would make libs/xbus depend on
// libs/gsof, which is the coupling bd992/byte_stream.h argues against, and
// half of what is below has no GSOF analogue at all.
//
// Reading past the end is the caller's problem to prevent. Every parser in
// this library checks its span size once, against kSize, before the first
// read.

#ifndef XBUS_BYTE_ORDER_H
#define XBUS_BYTE_ORDER_H

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xbus
{

// Unsigned integers, assembled most-significant byte first.
constexpr std::uint8_t read_u8(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return bytes[offset];
}

constexpr std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

constexpr std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

constexpr std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

// Signed integers. Conversion to a signed type is modular from C++20 onwards,
// so going through the unsigned read keeps two's-complement assembly out of
// the call sites.
constexpr std::int8_t read_i8(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::int8_t>(read_u8(bytes, offset));
}

constexpr std::int16_t read_i16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(read_u16(bytes, offset));
}

constexpr std::int32_t read_i32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

constexpr std::int64_t read_i64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::int64_t>(read_u64(bytes, offset));
}

// IEEE 754. std::bit_cast is constexpr, which is the whole reason the rest of
// this file exists.
constexpr float read_f32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(read_u32(bytes, offset));
}

constexpr double read_f64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<double>(read_u64(bytes, offset));
}

// ---------------------------------------------------------------------------
// The fixed-point formats.
//
// A single MTi output can be requested in any of four precisions, selected by
// the low two bits of its data identifier. Two of them are Xsens' own, and the
// second is the single most dangerous thing in this protocol.

// Fixed point 12.20: a 32-bit big-endian integer equal to round(v * 2^20).
// Range [-2048.0 .. 2047.9999990].
inline constexpr int kFp1220FractionBits = 20;

constexpr double read_fp1220(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    // 1.0 / 2^20, written as a division by a power of two so it is exact.
    return static_cast<double>(read_i32(bytes, offset)) / 1048576.0;
}

// Fixed point 16.32: round(v * 2^32) as a 64-bit integer, of which only the
// low SIX bytes are transmitted -- and NOT in order.
//
// With b0 the least significant byte of that integer, the wire order is
//
//     b3 b2 b1 b0 b5 b4
//
// which is the 32-bit fractional part big-endian, followed by the 16-bit
// integer part big-endian. Reading the six bytes as one big-endian quantity
// compiles, runs, and yields a plausible wrong number for every value -- and
// round-tripping through a matching encoder cannot detect it, because an
// encoder and decoder sharing the same wrong order agree perfectly. The check
// that does catch it is in tests/test_fixed_point.cpp: decode the same
// physical value from a float32 and from an fp16.32 encoding and require them
// to agree.
//
// The 48-bit value is signed, so the integer part must be sign-extended from
// bit 15 rather than zero-filled; an aircraft pitching nose-down is exactly
// where that shows up.
inline constexpr std::size_t kFp1632Size = 6;

constexpr double read_fp1632(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    const std::uint32_t fraction = read_u32(bytes, offset);
    const std::int32_t integer = static_cast<std::int16_t>(read_u16(bytes, offset + 4));

    // 1.0 / 2^32, exact.
    return static_cast<double>(integer) +
           static_cast<double>(fraction) / 4294967296.0;
}

// The write side exists for the command builders and for the test vectors,
// which construct packets as std::array at compile time.
constexpr void write_u8(std::span<std::uint8_t> bytes, std::size_t offset, std::uint8_t value)
{
    bytes[offset] = value;
}

constexpr void write_u16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xFFu);
}

constexpr void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFFu);
}

constexpr void write_f32(std::span<std::uint8_t> bytes, std::size_t offset, float value)
{
    write_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

// The encoders for the two fixed-point formats. These exist ONLY to build test
// vectors -- the MTi is what encodes on the wire, and nothing this library
// sends to a device carries a fixed-point value. They are here rather than in
// the test so that the swizzle is written down exactly once: a test-local
// encoder that got it wrong in the same way as the reader would agree with it.
constexpr void write_fp1220(std::span<std::uint8_t> bytes, std::size_t offset, double value)
{
    write_u32(bytes, offset,
              static_cast<std::uint32_t>(static_cast<std::int32_t>(value * 1048576.0 +
                                                                   (value < 0.0 ? -0.5 : 0.5))));
}

constexpr void write_fp1632(std::span<std::uint8_t> bytes, std::size_t offset, double value)
{
    const std::int64_t scaled =
        static_cast<std::int64_t>(value * 4294967296.0 + (value < 0.0 ? -0.5 : 0.5));

    write_u32(bytes, offset, static_cast<std::uint32_t>(static_cast<std::uint64_t>(scaled) & 0xFFFFFFFFull));
    write_u16(bytes, offset + 4, static_cast<std::uint16_t>((static_cast<std::uint64_t>(scaled) >> 32) & 0xFFFFull));
}

// A bit test, spelled out because the LLCP describes every flag word as
// "bit 0 .. bit 31" and a call site that reads `bit(status, 19)` needs no
// comment tying it back.
constexpr bool bit(std::uint32_t value, unsigned index)
{
    return ((value >> index) & 1u) != 0u;
}

} // namespace xbus

#endif // XBUS_BYTE_ORDER_H
