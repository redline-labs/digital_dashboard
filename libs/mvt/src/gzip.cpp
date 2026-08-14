// SPDX-License-Identifier: GPL-3.0-or-later
#include "mvt/gzip.h"

#include "mvt/decode.h"

#include <zlib.h>

#include <cstring>

namespace mvt
{
namespace
{

// 32 + 15 asks zlib to auto-detect gzip or zlib framing. Plain 15 handles only
// zlib and -15 only raw deflate; an archive written by tippecanoe and one
// written by tilemaker do not agree on which they use, so detecting is the only
// thing that reads both.
constexpr int kAutoDetectWindowBits = 32 + 15;

// A tile is a few hundred kilobytes at the very worst. Anything claiming to
// inflate past this is either corrupt or hostile, and inflate() will happily
// keep going otherwise -- a zip bomb is a small file that never stops.
constexpr std::size_t kMaxInflatedBytes = 64U * 1024U * 1024U;

constexpr std::size_t kChunk = 64U * 1024U;

} // namespace

Result<std::vector<std::uint8_t>> inflate(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return decompress_failed("empty input");
    }

    z_stream stream {};
    if (inflateInit2(&stream, kAutoDetectWindowBits) != Z_OK)
    {
        return decompress_failed("inflateInit2 failed");
    }

    // The input pointer is const on our side and not on zlib's; zlib does not
    // write through it.
    stream.next_in = const_cast<Bytef*>(bytes.data());
    stream.avail_in = static_cast<uInt>(bytes.size());

    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> chunk(kChunk);

    int status = Z_OK;
    do
    {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());

        status = ::inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
        {
            const std::string reason = (stream.msg != nullptr) ? stream.msg : "corrupt stream";
            inflateEnd(&stream);
            return decompress_failed(reason);
        }

        const std::size_t produced = chunk.size() - stream.avail_out;
        if (out.size() + produced > kMaxInflatedBytes)
        {
            inflateEnd(&stream);
            return decompress_failed("inflated past " + std::to_string(kMaxInflatedBytes) +
                                     " bytes");
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));

        // Z_BUF_ERROR with nothing produced and nothing left to read means the
        // stream is truncated -- zlib cannot make progress and is not going to
        // start. Without this the loop spins forever.
        if (status == Z_BUF_ERROR && produced == 0)
        {
            inflateEnd(&stream);
            return decompress_failed("truncated stream");
        }
    } while (status != Z_STREAM_END);

    inflateEnd(&stream);
    return out;
}

Result<std::vector<std::uint8_t>> inflateIfCompressed(std::span<const std::uint8_t> bytes)
{
    if (!looksCompressed(bytes))
    {
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
    return inflate(bytes);
}

} // namespace mvt
