// SPDX-License-Identifier: GPL-3.0-or-later
//
// Inflate, for tiles that arrive compressed.
//
// The mbtiles spec says vector tiles MUST be gzipped, and nodes/map_server
// ships them exactly as the archive stored them -- inflating on the server only
// to send three times the bytes would spend CPU at both ends to make the wire
// worse. So the client inflates, here.
//
// Only inflate. Nothing in this tree compresses a tile.
#ifndef MVT_GZIP_H
#define MVT_GZIP_H

#include <cstdint>
#include <span>
#include <vector>

#include "mvt/error.h"

namespace mvt
{

// Inflate a gzip or zlib stream. Auto-detects which, so a caller does not have
// to care which one the archive used -- and real archives use both.
//
// Passing UNCOMPRESSED bytes is an error rather than a pass-through: silently
// returning the input would make a corrupt tile and a plain tile
// indistinguishable at this layer. Use inflateIfCompressed() when the input may
// legitimately be either.
Result<std::vector<std::uint8_t>> inflate(std::span<const std::uint8_t> bytes);

// Inflate when the bytes look compressed, copy them through when they do not.
// What the tile path actually wants: the server reports the encoding, but the
// archive's metadata and its blobs disagree often enough that sniffing wins.
Result<std::vector<std::uint8_t>> inflateIfCompressed(std::span<const std::uint8_t> bytes);

} // namespace mvt

#endif // MVT_GZIP_H
