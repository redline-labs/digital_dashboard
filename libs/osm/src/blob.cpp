// SPDX-License-Identifier: GPL-3.0-or-later
#include "osm/blob.h"

#include <zlib.h>

#include "protowire/reader.h"

namespace osm
{
namespace
{

// BlobHeader field numbers.
constexpr std::uint32_t kHeaderType = 1;
constexpr std::uint32_t kHeaderDatasize = 3;

// Blob field numbers.
constexpr std::uint32_t kBlobRaw = 1;
constexpr std::uint32_t kBlobRawSize = 2;
constexpr std::uint32_t kBlobZlibData = 3;
constexpr std::uint32_t kBlobLzmaData = 4;
constexpr std::uint32_t kBlobBzip2Data = 5;
constexpr std::uint32_t kBlobLz4Data = 6;
constexpr std::uint32_t kBlobZstdData = 7;

BlobKind kindOf(std::string_view type)
{
    if (type == "OSMHeader")
    {
        return BlobKind::Header;
    }
    if (type == "OSMData")
    {
        return BlobKind::Data;
    }
    return BlobKind::Unknown;
}

} // namespace

Result<Blob> BlobIterator::next()
{
    const std::size_t start = mAt;

    // THE ONLY BIG-ENDIAN NUMBER IN THE FILE. Everything inside a blob is
    // protobuf and therefore little-endian varints; this one length prefix is
    // network order. Reading it the other way round yields a plausible length
    // (0x0D000000 rather than 0x0D) and the first blob parses before everything
    // after it is garbage -- which reads as a corrupt file rather than as a
    // byte-order bug.
    if (mFile.size() - mAt < 4)
    {
        return truncated("blob header length prefix", mAt);
    }

    const std::uint32_t headerLength = (static_cast<std::uint32_t>(mFile[mAt]) << 24) |
                                       (static_cast<std::uint32_t>(mFile[mAt + 1]) << 16) |
                                       (static_cast<std::uint32_t>(mFile[mAt + 2]) << 8) |
                                       static_cast<std::uint32_t>(mFile[mAt + 3]);
    mAt += 4;

    if (headerLength > kMaxBlobHeaderBytes)
    {
        return malformed("blob header of " + std::to_string(headerLength) +
                             " bytes exceeds the 64 KiB the format allows",
                         start);
    }
    if (mFile.size() - mAt < headerLength)
    {
        return truncated("blob header of " + std::to_string(headerLength) + " bytes", start);
    }

    const auto headerBytes = mFile.subspan(mAt, headerLength);
    mAt += headerLength;

    std::string type;
    std::int32_t datasize = -1;

    protowire::Reader header(headerBytes);
    while (!header.done())
    {
        auto field = header.field();
        if (!field)
        {
            return from_wire(field.error(), start);
        }

        if (field->number == kHeaderType && field->wire == protowire::WireType::LengthDelimited)
        {
            auto text = header.text();
            if (!text)
            {
                return from_wire(text.error(), start);
            }
            type.assign(*text);
        }
        else if (field->number == kHeaderDatasize && field->wire == protowire::WireType::Varint)
        {
            // int32, not uint32: a negative datasize would otherwise become
            // 4 billion and size the read below.
            auto size = header.int32();
            if (!size)
            {
                return from_wire(size.error(), start);
            }
            datasize = *size;
        }
        else
        {
            if (auto skipped = header.skip(field->wire); !skipped)
            {
                return from_wire(skipped.error(), start);
            }
        }
    }

    if (datasize < 0)
    {
        return malformed("blob header with datasize " + std::to_string(datasize), start);
    }
    if (static_cast<std::uint32_t>(datasize) > kMaxBlobBytes)
    {
        return malformed("blob of " + std::to_string(datasize) +
                             " bytes exceeds the 32 MiB the format allows",
                         start);
    }
    if (mFile.size() - mAt < static_cast<std::size_t>(datasize))
    {
        return truncated("blob body of " + std::to_string(datasize) + " bytes", start);
    }

    Blob blob;
    blob.type = type;
    blob.kind = kindOf(type);
    blob.message = mFile.subspan(mAt, static_cast<std::size_t>(datasize));
    blob.offset = start;

    mAt += static_cast<std::size_t>(datasize);
    return blob;
}

Result<void> inflateBlob(const Blob& blob, std::vector<std::uint8_t>& out)
{
    out.clear();

    std::span<const std::uint8_t> raw;
    std::span<const std::uint8_t> zlibData;
    std::int32_t rawSize = -1;
    const char* unsupportedCodec = nullptr;

    protowire::Reader reader(blob.message);
    while (!reader.done())
    {
        auto field = reader.field();
        if (!field)
        {
            return from_wire(field.error(), blob.offset);
        }

        switch (field->number)
        {
            case kBlobRaw:
            {
                auto bytes = reader.bytes();
                if (!bytes)
                {
                    return from_wire(bytes.error(), blob.offset);
                }
                raw = *bytes;
                break;
            }
            case kBlobRawSize:
            {
                auto size = reader.int32();
                if (!size)
                {
                    return from_wire(size.error(), blob.offset);
                }
                rawSize = *size;
                break;
            }
            case kBlobZlibData:
            {
                auto bytes = reader.bytes();
                if (!bytes)
                {
                    return from_wire(bytes.error(), blob.offset);
                }
                zlibData = *bytes;
                break;
            }
            // Named individually rather than lumped together, because the
            // message is the whole value: "this file uses zstd" tells you to
            // re-export it, "unsupported blob" tells you nothing.
            case kBlobLzmaData:
                unsupportedCodec = "lzma";
                break;
            case kBlobBzip2Data:
                unsupportedCodec = "bzip2";
                break;
            case kBlobLz4Data:
                unsupportedCodec = "lz4";
                break;
            case kBlobZstdData:
                unsupportedCodec = "zstd";
                break;
            default:
                break;
        }

        if (unsupportedCodec != nullptr)
        {
            return unsupported(std::string("blob compressed with ") + unsupportedCodec +
                                   ", which this build does not implement",
                               blob.offset);
        }

        if (field->number != kBlobRaw && field->number != kBlobRawSize &&
            field->number != kBlobZlibData)
        {
            if (auto skipped = reader.skip(field->wire); !skipped)
            {
                return from_wire(skipped.error(), blob.offset);
            }
        }
    }

    if (!raw.empty())
    {
        out.assign(raw.begin(), raw.end());
        return {};
    }

    if (zlibData.empty())
    {
        return malformed("blob with no payload", blob.offset);
    }
    if (rawSize <= 0)
    {
        return malformed("compressed blob with raw_size " + std::to_string(rawSize), blob.offset);
    }
    if (static_cast<std::uint32_t>(rawSize) > kMaxBlobBytes)
    {
        return malformed("blob inflating to " + std::to_string(rawSize) + " bytes", blob.offset);
    }

    // Sized exactly from raw_size rather than grown. The whole point of the
    // caller-owned buffer is that a million blocks cost no allocations after
    // the first few; a growth loop would give that back.
    out.resize(static_cast<std::size_t>(rawSize));

    z_stream stream {};
    stream.next_in = const_cast<Bytef*>(zlibData.data());
    stream.avail_in = static_cast<uInt>(zlibData.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    // Window bits 15: zlib-wrapped, which is what the format says. NOT 15+16
    // (gzip) and not -15 (raw) -- mvt::inflate auto-detects because tiles vary,
    // and a PBF does not.
    if (inflateInit2(&stream, 15) != Z_OK)
    {
        return decompress_failed("zlib would not start", blob.offset);
    }

    const int status = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);

    if (status != Z_STREAM_END)
    {
        return decompress_failed("zlib refused the blob (code " + std::to_string(status) + ")",
                                 blob.offset);
    }
    if (produced != out.size())
    {
        // raw_size is what sized the buffer and what the block's field offsets
        // will be read against, so a disagreement is not cosmetic.
        return malformed("blob declared " + std::to_string(out.size()) + " bytes but inflated to " +
                             std::to_string(produced),
                         blob.offset);
    }

    return {};
}

} // namespace osm
