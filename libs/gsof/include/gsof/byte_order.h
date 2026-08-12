// SPDX-License-Identifier: GPL-3.0-or-later
//
// Big-endian scalar reads and writes over a span of bytes.
//
// Everything Trimble puts on the wire is big-endian, and the obvious C++ answer
// -- a #pragma pack struct, a memcpy, and a byteswap of each member -- is the
// one this library deliberately does not use. Three reasons, in order of how
// much they cost:
//
//   1. memcpy is not usable in constant evaluation, so a packed-struct parser
//      cannot be constexpr, and every field offset in this protocol then has to
//      be verified by running a test rather than by building one. A wrong
//      offset produces a plausible latitude, not a crash, so "the test passed"
//      and "the offsets are right" are different statements unless the check
//      happens at compile time.
//   2. A byteswap-on-read parser is only correct on a little-endian host. It
//      compiles and silently produces garbage on anything else. Assembling from
//      individual bytes is correct everywhere and, at -O2, compiles to the same
//      single load-and-swap instruction.
//   3. Packed structs mean unaligned member access, which is UB the sanitizers
//      complain about even where the hardware tolerates it.
//
// Reading past the end is the caller's problem to prevent: these functions are
// the innermost loop of every record parser, and a bounds check per field would
// be a check per field for a bound the record parser has already established
// once from its length. Every caller in this library checks the span size
// before the first read -- see the kSize check at the top of each parse().

#ifndef GSOF_BYTE_ORDER_H
#define GSOF_BYTE_ORDER_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gsof
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

// Signed integers. Going through the unsigned read and casting is well defined
// from C++20 onwards (conversion to a signed type is modular, not
// implementation-defined), and it keeps two's-complement assembly out of the
// call sites.
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

// IEEE 754, which is what the ICD means by FLOAT and DOUBLE. std::bit_cast is
// constexpr, which is the whole reason the rest of this file exists.
constexpr float read_f32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(read_u32(bytes, offset));
}

constexpr double read_f64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<double>(read_u64(bytes, offset));
}

// The write side exists for the command builders, which construct packets as
// std::array at compile time.
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

// A bit test, spelled out because the ICD describes every flag byte as
// "bit 0 .. bit 7" and a call site that reads `bit(flags, 3)` needs no comment
// tying it back.
constexpr bool bit(std::uint8_t value, unsigned index)
{
    return ((value >> index) & 1u) != 0u;
}

} // namespace gsof

#endif // GSOF_BYTE_ORDER_H
