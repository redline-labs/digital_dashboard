// SPDX-License-Identifier: GPL-3.0-or-later
#include "protowire/reader.h"

#include <limits>

namespace protowire
{
namespace
{

// A varint is at most ten bytes: 64 bits in 7-bit groups is ceil(64/7) = 10.
constexpr int kMaxVarintBytes = 10;

} // namespace

Result<std::uint8_t> Reader::byte()
{
    if (mAt >= mBytes.size())
    {
        return truncated("ran out of bytes", mAt);
    }
    return mBytes[mAt++];
}

Result<std::uint64_t> Reader::varint()
{
    const std::size_t start = mAt;

    std::uint64_t value = 0;
    for (int i = 0; i < kMaxVarintBytes; ++i)
    {
        auto next = byte();
        if (!next)
        {
            return std::unexpected(next.error());
        }

        value |= static_cast<std::uint64_t>(*next & 0x7F) << (7 * i);
        if ((*next & 0x80) == 0)
        {
            return value;
        }
    }

    // Continuation bit still set after ten bytes. Continuing would shift past
    // 64 and silently wrap, producing a number that is merely wrong.
    return malformed("varint longer than 10 bytes", start);
}

Result<std::int64_t> Reader::zigzag()
{
    auto raw = varint();
    if (!raw)
    {
        return std::unexpected(raw.error());
    }
    return unzigzag(*raw);
}

Result<std::int64_t> Reader::int64()
{
    auto raw = varint();
    if (!raw)
    {
        return std::unexpected(raw.error());
    }
    // Every 64-bit pattern is a legal int64, so this is a reinterpretation
    // rather than a conversion -- and it is written out because the implicit
    // narrowing it replaces is exactly the bug this function exists to prevent.
    return static_cast<std::int64_t>(*raw);
}

Result<std::int32_t> Reader::int32()
{
    const std::size_t start = mAt;

    auto wide = int64();
    if (!wide)
    {
        return std::unexpected(wide.error());
    }

    // A proto int32 travels sign-extended to 64 bits, so anything outside the
    // 32-bit range is a field that is not the int32 we were told it was.
    if (*wide < std::numeric_limits<std::int32_t>::min() ||
        *wide > std::numeric_limits<std::int32_t>::max())
    {
        return malformed("int32 field carrying " + std::to_string(*wide), start);
    }

    return static_cast<std::int32_t>(*wide);
}

Result<std::uint32_t> Reader::fixed32()
{
    if (remaining() < 4)
    {
        return truncated("fixed32 past the end", mAt);
    }

    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
    {
        value |= static_cast<std::uint32_t>(mBytes[mAt + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    mAt += 4;
    return value;
}

Result<std::uint64_t> Reader::fixed64()
{
    if (remaining() < 8)
    {
        return truncated("fixed64 past the end", mAt);
    }

    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value |= static_cast<std::uint64_t>(mBytes[mAt + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    mAt += 8;
    return value;
}

Result<Field> Reader::field()
{
    const std::size_t start = mAt;

    auto tag = varint();
    if (!tag)
    {
        return std::unexpected(tag.error());
    }

    const std::uint64_t number = *tag >> 3;
    const std::uint64_t wire = *tag & 0x07;

    // Field 0 is not a legal field number, and it is what a run of zero bytes
    // decodes to -- so this is the check that stops padding or a zeroed buffer
    // reading as an endless stream of empty varint fields.
    if (number == 0)
    {
        return malformed("field number 0", start);
    }
    if (number > 0xFFFFFFFFULL)
    {
        return malformed("field number does not fit 32 bits", start);
    }
    if (wire > static_cast<std::uint64_t>(WireType::Fixed32))
    {
        return malformed("wire type " + std::to_string(wire), start);
    }

    return Field { static_cast<std::uint32_t>(number), static_cast<WireType>(wire) };
}

Result<std::span<const std::uint8_t>> Reader::bytes()
{
    const std::size_t start = mAt;

    auto length = varint();
    if (!length)
    {
        return std::unexpected(length.error());
    }

    if (*length > remaining())
    {
        return truncated("length-delimited field of " + std::to_string(*length) +
                             " bytes with only " + std::to_string(remaining()) + " left",
                         start);
    }

    const auto view = mBytes.subspan(mAt, static_cast<std::size_t>(*length));
    mAt += static_cast<std::size_t>(*length);
    return view;
}

Result<std::string_view> Reader::text()
{
    auto view = bytes();
    if (!view)
    {
        return std::unexpected(view.error());
    }
    return std::string_view(reinterpret_cast<const char*>(view->data()), view->size());
}

Result<Reader> Reader::sub()
{
    auto view = bytes();
    if (!view)
    {
        return std::unexpected(view.error());
    }
    return Reader(*view);
}

Result<void> Reader::skip(WireType wire)
{
    switch (wire)
    {
        case WireType::Varint:
        {
            auto value = varint();
            if (!value)
            {
                return std::unexpected(value.error());
            }
            return {};
        }

        case WireType::Fixed64:
        {
            auto value = fixed64();
            if (!value)
            {
                return std::unexpected(value.error());
            }
            return {};
        }

        case WireType::LengthDelimited:
        {
            auto value = bytes();
            if (!value)
            {
                return std::unexpected(value.error());
            }
            return {};
        }

        case WireType::Fixed32:
        {
            auto value = fixed32();
            if (!value)
            {
                return std::unexpected(value.error());
            }
            return {};
        }

        case WireType::StartGroup:
        case WireType::EndGroup:
            // Groups were deprecated in proto2 and MVT does not use them.
            // Skipping one means matching it to its end tag, which is more
            // machinery than a format that cannot contain them deserves.
            return unsupported("protobuf groups", mAt);
    }

    return malformed("unknown wire type", mAt);
}

} // namespace protowire
