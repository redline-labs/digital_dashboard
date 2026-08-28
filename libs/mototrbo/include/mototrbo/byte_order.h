// SPDX-License-Identifier: GPL-3.0-or-later
//
// Big-endian scalar reads and writes over a span of bytes.
//
// XNL and XCMP are network order throughout. The reasoning for assembling
// values a byte at a time rather than memcpy-ing a packed struct is the same
// one gsof/byte_order.h sets out at length: memcpy is not usable in constant
// evaluation, and everything in this library is constexpr so that a captured
// frame plus a static_assert turns a wrong offset into a build error.
//
// Deliberately a second, smaller copy rather than a dependency on
// gsof/byte_order.h. This library links nothing -- see the note in its
// CMakeLists -- and taking a Trimble GNSS library along for four functions
// would be a worse trade than thirty lines.
//
// Bounds are the caller's problem: every parser here checks the span size once
// before its first read.

#ifndef MOTOTRBO_BYTE_ORDER_H
#define MOTOTRBO_BYTE_ORDER_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace mototrbo
{

constexpr std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

constexpr std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
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

} // namespace mototrbo

#endif // MOTOTRBO_BYTE_ORDER_H
