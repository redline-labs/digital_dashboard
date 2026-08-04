// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading and writing a field at a bit offset inside a CAN frame.
//
// Generated PDO accessors call these rather than shifting bytes themselves, so
// the packing rule lives in one place. CANopen packs a PDO little-endian: the
// first mapped entry occupies the least significant bits of byte 0, and each
// entry that follows starts where the previous one ended. A byte-aligned
// 8/16/32-bit field -- which is every field on this device -- reduces to a
// plain little-endian load, but the general form costs nothing extra and means
// a device that maps a 4-bit flag needs no new code.
#ifndef CANOPEN_PDO_BITS_H
#define CANOPEN_PDO_BITS_H

#include <cstdint>
#include <span>

namespace canopen
{

// Extract `bits` bits starting at `bitOffset`. Bits that fall outside the
// buffer read as zero rather than as whatever follows it in memory -- a short
// frame yields zeroed fields instead of adjacent stack.
inline uint64_t get_bits(std::span<const uint8_t> data, uint8_t bitOffset, uint8_t bits)
{
    uint64_t value = 0;
    for (uint8_t i = 0; i < bits; ++i)
    {
        const uint16_t bit = static_cast<uint16_t>(bitOffset + i);
        const size_t byte = bit / 8u;
        if (byte >= data.size())
        {
            break;
        }
        if ((data[byte] >> (bit % 8u)) & 1u)
        {
            value |= (uint64_t { 1 } << i);
        }
    }
    return value;
}

inline void set_bits(std::span<uint8_t> data, uint8_t bitOffset, uint8_t bits, uint64_t value)
{
    for (uint8_t i = 0; i < bits; ++i)
    {
        const uint16_t bit = static_cast<uint16_t>(bitOffset + i);
        const size_t byte = bit / 8u;
        if (byte >= data.size())
        {
            break;
        }
        const uint8_t mask = static_cast<uint8_t>(1u << (bit % 8u));
        if ((value >> i) & 1u)
        {
            data[byte] = static_cast<uint8_t>(data[byte] | mask);
        }
        else
        {
            data[byte] = static_cast<uint8_t>(data[byte] & ~mask);
        }
    }
}

// Sign-extend a `bits`-wide two's complement value that has been read into the
// low bits of a uint64_t. Needed for the INTEGER* types; the device's
// brightness objects are INTEGER16 even though their range is non-negative.
inline int64_t sign_extend(uint64_t value, uint8_t bits)
{
    if (bits == 0 || bits >= 64)
    {
        return static_cast<int64_t>(value);
    }
    const uint64_t signBit = uint64_t { 1 } << (bits - 1);
    if ((value & signBit) != 0)
    {
        value |= ~((uint64_t { 1 } << bits) - 1);
    }
    return static_cast<int64_t>(value);
}

} // namespace canopen

#endif // CANOPEN_PDO_BITS_H
