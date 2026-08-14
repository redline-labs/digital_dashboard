// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bytes in, a Tile out.
//
// The whole library's entry point. Everything else is either what it produces
// (mvt/tile.h) or how it reads the wire (mvt/reader.h).
#ifndef MVT_DECODE_H
#define MVT_DECODE_H

#include <cstdint>
#include <span>

#include "mvt/error.h"
#include "mvt/tile.h"

namespace mvt
{

// Decode an UNCOMPRESSED vector tile.
//
// .mbtiles archives store these gzipped; nodes/map_server ships them that way
// deliberately, so the caller inflates first -- see mvt/gzip.h. Handing this
// function a gzip stream does not fail loudly: protobuf reads 0x1F as field 3,
// wire type 7, and the error it eventually produces names a wire type rather
// than the real problem, so gzipped input is checked for by name below.
Result<Tile> decode(std::span<const std::uint8_t> bytes);

// True when the buffer begins with a gzip or zlib header. Used by decode() to
// turn "you forgot to inflate" into a message that says so.
bool looksCompressed(std::span<const std::uint8_t> bytes);

} // namespace mvt

#endif // MVT_DECODE_H
