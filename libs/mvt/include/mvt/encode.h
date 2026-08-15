// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writing a Mapbox Vector Tile.
//
// The mirror of decode.h, and it lives beside it on purpose: the encode/decode
// ROUND TRIP is the strongest test either has, and putting the two in one
// library means that test cannot drift into a corner where only one of them is
// exercised.
//
// Everything decode.h warns about applies here in reverse:
//
//   * The geometry cursor persists across commands, so every point is written
//     as a delta from the previous one. Writing absolutes produces a tile that
//     decodes into a shape crumpled onto the tile corner.
//
//   * Polygon ring winding decides exterior from hole. This does NOT reorder or
//     rewind rings -- the caller owns that decision, because only the caller
//     knows which ring is a hole -- but mvt::signedArea() is there to check.
//
//   * `extent` is per layer and is written out even when it is the default,
//     because a tile that omits it and a tile that says 4096 are the same tile,
//     and being explicit costs three bytes per layer.
#ifndef MVT_ENCODE_H
#define MVT_ENCODE_H

#include <cstdint>
#include <span>
#include <vector>

#include "mvt/error.h"
#include "mvt/tile.h"

namespace mvt
{

// Serialise a tile to MVT protobuf. Not compressed -- gzip is the archive's
// business, and mbtiles wants to decide that for itself.
Result<std::vector<std::uint8_t>> encode(const Tile& tile);

// Gzip, for a tile on its way into an .mbtiles.
//
// The spec says vector tiles are stored gzipped, and every consumer in this
// tree inflates before decoding -- including the widget, which is why the
// server passes them through untouched.
Result<std::vector<std::uint8_t>> gzipCompress(std::span<const std::uint8_t> bytes);

} // namespace mvt

#endif // MVT_ENCODE_H
