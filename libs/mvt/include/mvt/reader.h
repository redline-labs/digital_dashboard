// SPDX-License-Identifier: GPL-3.0-or-later
//
// Just enough protobuf to read a vector tile.
//
// A vector tile is protobuf, and we do not link a protobuf library for it. The
// .proto is eleven fields across four messages and has not changed since 2016,
// the encoding is four wire types of which MVT uses two, and pulling in libprotoc
// plus a generated-code step for that would be a larger dependency than the
// format it reads. This tree already hand-decodes considerably worse -- GSOF,
// iAP2, usbmux, the CANopen EDS grammar.
//
// What this is NOT is a general protobuf implementation. It reads fields in the
// order they appear, it does not build a message object, and it knows nothing
// about required/optional/default beyond what the caller applies.
#ifndef MVT_READER_H
#define MVT_READER_H

#include <cstdint>
#include <span>
#include <string_view>

#include "mvt/error.h"

namespace mvt
{

// Protobuf wire types. MVT uses only Varint and LengthDelimited; the other two
// are named so an unknown field carrying one can be SKIPPED rather than
// rejected. Skipping unknown fields is the forward-compatibility rule in the
// spec, and a decoder that errors on them breaks on the next spec revision.
enum class WireType : std::uint8_t
{
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
};

struct Field
{
    std::uint32_t number { 0 };
    WireType wire { WireType::Varint };
};

class Reader
{
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) : mBytes(bytes) {}

    bool done() const { return mAt >= mBytes.size(); }
    std::size_t offset() const { return mAt; }
    std::size_t remaining() const { return mBytes.size() - mAt; }

    // Read the next tag. Only call when !done().
    Result<Field> field();

    // Varints are up to ten bytes: a 64-bit value needs ceil(64/7) = 10 groups.
    // A continuation bit still set on the tenth byte means the encoder was wrong
    // or the buffer is not what we think it is, and continuing would silently
    // wrap.
    Result<std::uint64_t> varint();

    Result<std::int64_t> zigzag();
    Result<std::uint32_t> fixed32();
    Result<std::uint64_t> fixed64();

    // The bytes of a length-delimited field, borrowed from the buffer. Valid as
    // long as the buffer is.
    Result<std::span<const std::uint8_t>> bytes();
    Result<std::string_view> text();

    // Everything a length-delimited field contains, as its own Reader. This is
    // how nested messages and packed repeated fields are read.
    Result<Reader> sub();

    // Advance past a field whose value we do not want. Required for unknown
    // fields; the wire type is what makes it possible to skip one at all.
    Result<void> skip(WireType wire);

  private:
    Result<std::uint8_t> byte();

    std::span<const std::uint8_t> mBytes;
    std::size_t mAt { 0 };
};

// Zigzag: (n >> 1) ^ -(n & 1). Written out because the naive `n / 2 * sign` a
// reader might expect is wrong for negative values, and because getting it
// wrong yields geometry that is plausible and mirrored rather than an error.
constexpr std::int64_t unzigzag(std::uint64_t value)
{
    return static_cast<std::int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

} // namespace mvt

#endif // MVT_READER_H
