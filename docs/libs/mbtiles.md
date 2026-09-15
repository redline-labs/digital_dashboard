---
title: mbtiles
parent: Libraries
---

# mbtiles

## Overview

`libs/mbtiles` is the mbtiles 1.3 format and nothing else: a read-only
`Archive`, the `metadata` table parsed, the `Writer` the tiler uses, and the
sniff that says what a tile blob is compressed with. No zenoh, no Qt, no
network. Everything here is answerable from a file on disk;
[map_server](../nodes/map_server.html) is what puts the answers on the bus,
`tools/map_build` ([Building the map](../tools/map_build.html)) is what writes
the files, and `scope` is the one GUI app that opens them directly. The tiles
it hands back are decoded by [mvt](mvt.html) and drawn by
[map_render](map_render.html), neither of which links this library.

## Public headers

| Header | What it holds |
|---|---|
| `archive.h` | `Archive`: `open()`, `metadata()`, `tile(z, x, y)` in XYZ; the `Tile` it returns |
| `metadata.h` | The `metadata` table, every optional field with a "was it there" answer |
| `writer.h` | `Writer`; used only by `tools/map_build` |
| `compression.h` | `Encoding`, sniffed from the blob's magic bytes |
| `error.h` | `Error`, and `Result<T>` as `std::expected<T, Error>` |

## Using it

The target is `mbtiles`. It links the vendored `sqlite3` PRIVATE, and
`nlohmann_json` PRIVATE for assembling the TileJSON document only; the
archive's own `json` column is passed through rather than interpreted.

Nothing here decompresses. The client has to inflate before it can decode
anyway, and inflating a tile on the server only to re-gzip it, or worse send it
raw, would spend CPU at both ends to triple the payload. `compression.h` sniffs
the blob rather than trusting anything, because the spec says vector tiles
"MUST be gzip-compressed" and archives in the wild are not all obedient:
tilemaker, tippecanoe and hand-assembled files disagree, and the `metadata`
table has no column for it. The answer rides on the wire beside the bytes.

The spec requires only `name` and `format` in `metadata`; real archives leave
most of the rest out. So every field has a "was it there at all" answer rather
than a plausible default: absent bounds are not the whole world and an absent
centre is not null island. A client that cannot tell the difference will fly
the camera to 0,0.

{: .warning }
The vendored `sqlite3` stays out of the dashboard and the editor. Qt reaches
SQLite through `Qt6::Sql`, which links its own copy, and the two must never
meet in one process. `scope` is the deliberate exception: it opens archives
itself and links no `Qt6::Sql`. Check with
`grep -o sqlite3 build/apps/dashboard/CMakeFiles/dashboard.dir/link.txt`,
which must stay empty.

## Behaviour worth knowing

**XYZ in, TMS on disk.** The mbtiles spec stores TMS rows: `tile_row` counts
northward from the bottom. Everything else in the tree, the projection, the
capnp request and the widget, uses XYZ (slippy), where `y` counts southward
from the top. The conversion is `row = 2^z - 1 - y`, and it happens in exactly
two places: `Archive::tile()` on the way out and `Writer::put()` on the way in.
Nowhere else in the tree may flip a tile coordinate.

This gets its own paragraph because of how it fails. A wrong flip does not
throw, log or return nothing. It returns a real tile from the wrong hemisphere,
and the map renders beautifully, mirrored about the equator, which nobody reads
as a coordinate bug.

**`tiles` is often a view.** The spec's deduplicating layout stores blobs in
`images` and the grid in `map`, joined by a view named `tiles`. Querying
`tiles` works for both, so nothing here may assume a table, and the tests build
one archive of each shape to keep it that way.

**A missing tile is not an error.** Most of a tile pyramid is empty, so
`tile()` reports absence through `Result<T>` and `spdlog` is deliberately not
linked; whether a miss is worth a log line is the server's decision.

## Tests

`mbtiles_test_archive` pins the TMS/XYZ flip by writing archives in TMS and
asserting in XYZ (inverting the flip in `archive.cpp` makes 20 checks fail),
plus both archive layouts and the concurrency the server subjects one
`Archive` to. `mbtiles_test_metadata` covers the metadata table and the
TileJSON built from it, which is mostly about absence and malformation. The
tests build their own archives with sqlite rather than shipping a fixture, so
they link `sqlite3` themselves; the library never writes an archive outside
`Writer`.
