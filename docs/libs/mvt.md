---
title: mvt
parent: Libraries
---

# mvt

## Overview

`libs/mvt` decodes and encodes Mapbox Vector Tiles: bytes in, geometry and
attributes out, and the reverse for the tiler. It is a minimal protobuf reader
(`libs/protowire`) plus the MVT geometry grammar, with no protobuf library
behind it. The `.proto` is eleven fields across four messages, unchanged since
2016, and pulling in libprotoc plus a codegen step for that would be a larger
dependency than the format it reads. No Qt, no zenoh, no I/O: everything here
is reachable from a buffer, so all of it is testable against real tiles pulled
out of a real archive without a display or a bus.

It exists because the renderer is ours; why is in
[Map renderer](../design/map-renderer.html). Its output is what
[map_render](map_render.html) tessellates, its input is what
[mbtiles](mbtiles.html) reads off disk and [map_server](../nodes/map_server.html)
ships over the bus, and its encoder is what `tools/map_build`
([Building the map](../tools/map_build.html)) writes archives with.

## Public headers

| Header | What it holds |
|---|---|
| `decode.h` | `mvt::decode`: bytes to a `Tile`. The library's entry point |
| `tile.h` | The decoded `Tile`, layers, features, tile-local integer geometry |
| `encode.h` | Writing a tile; `signedArea2()` in `tile.h` checks ring winding |
| `gzip.h` | `inflate` and `inflateIfCompressed`, for tiles that arrive compressed |
| `reader.h`, `error.h` | Aliases re-exporting `protowire`'s reader and `Result` types |

## Using it

The target is `mvt`; it links `protowire` PUBLIC because `error.h` and
`reader.h` re-export its types, and `ZLIB` PRIVATE for inflate. Tiles are
stored gzipped in an `.mbtiles` and travel that way, because inflating on the
server only to send three times the bytes would spend CPU at both ends to make
the wire worse, so the client inflates first and then decodes.

Coordinates stay in tile-local integer space, `0..extent`, exactly as the wire
has them. They are not projected here: the projection depends on where the
tile is drawn and at what size, which is the renderer's business, and
converting twice is how a map ends up subtly offset.

Only inflate lives here. Deflate is in `encode.h`, the one place a tile is
written, and is separate because inflating is on the hot path of every
viewport while deflating happens once, offline, in `map_build`.

## Behaviour worth knowing

Five things about the format are easy to get subtly wrong, and all five render
rather than fail.

| Trap | What it looks like |
|---|---|
| `extent` is per layer, default 4096 | one layer drawn at the wrong scale |
| Coordinates may fall outside `0..extent` (tile buffer) | a seam down every tile boundary |
| Polygon ring winding decides exterior from hole | every lake with an island fills solid |
| The geometry cursor persists across commands | every road collapses onto the tile corner |
| `ClosePath` does not repeat the first point | polygons drawn with one edge missing |

In MVT's y-down coordinate system a positive signed area is an exterior ring
and a negative one is a hole. The encoder does not reorder or rewind rings, as
only the caller knows which ring is a hole, but `signedArea2()` is there to
check. It writes `extent` even when it is the default, because a tile that
omits it and a tile that says 4096 are the same tile and being explicit costs
three bytes per layer.

{: .note }
`spdlog` is deliberately absent. The library reports through `Result<T>`, and
whether one bad tile in a viewport of forty is worth a log line is the
widget's decision.

## Tests

`mvt_test_decode` covers each trap in the table from a tile it builds itself.
`mvt_test_encode` encodes, decodes and requires the two to agree, which is the
strongest test either direction has: an encoder bug a decoder mirrors would
pass a one-sided test and produce tiles no other renderer could read.
`mvt_test_real_tiles` decodes real output from `MVT_TEST_ARCHIVE`, which CMake
sets to `socal-260813.mbtiles` under `-DREDLINE_MAP_DATA_DIR=<dir>`. It skips
loudly without one so a fresh checkout still passes, and is labelled `slow`,
not `unit`, because it reads a file the tree does not contain. On a machine that has it, this is the only test
that proves the decoder against bytes nobody here wrote; the Irvine tile at
`z14/2828/6562` is 81958 bytes, gzip, 14 layers.
