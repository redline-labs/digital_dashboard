// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a tile blob is compressed with, decided by looking at it.
//
// The mbtiles spec says vector tiles "MUST be gzip-compressed", and archives in
// the wild are not all obedient -- tilemaker, tippecanoe and hand-assembled
// files disagree, and the `metadata` table has no column for it at all. So this
// sniffs the blob rather than trusting anything, and the answer rides on the
// wire beside the bytes.
//
// Nothing here decompresses. The client has to inflate before it can decode
// anyway, and inflating a tile on the server only to re-gzip it (or worse, send
// it raw) would spend CPU at both ends to triple the payload.
#ifndef MBTILES_COMPRESSION_H
#define MBTILES_COMPRESSION_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace mbtiles
{

enum class Encoding : std::uint8_t
{
    Identity,
    Gzip,
    Deflate,
    Zstd,
};

const char* to_string(Encoding encoding);

// Sniff a blob's container.
//
// Deliberately conservative: only the containers whose magic bytes cannot
// plausibly begin an uncompressed tile are reported. A raw PBF starts with a
// protobuf field tag, and a PNG with its own 8-byte signature, so neither is
// mistaken for a compressed stream.
constexpr Encoding sniff(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B)
    {
        return Encoding::Gzip;
    }

    if (bytes.size() >= 4 && bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F &&
        bytes[3] == 0xFD)
    {
        return Encoding::Zstd;
    }

    // zlib: a CMF byte whose low nibble is 8 (deflate), and a two-byte header
    // that is a multiple of 31. Both checks are needed -- 0x78 alone also
    // begins plenty of legitimate protobuf.
    if (bytes.size() >= 2 && (bytes[0] & 0x0F) == 0x08 &&
        (((static_cast<unsigned>(bytes[0]) << 8) | bytes[1]) % 31U) == 0U)
    {
        return Encoding::Deflate;
    }

    return Encoding::Identity;
}

} // namespace mbtiles

#endif // MBTILES_COMPRESSION_H
