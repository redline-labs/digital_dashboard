---
title: osm
parent: Libraries
---

# osm

## Overview

OpenStreetMap PBF, decoded. This is the source everything on the map side is
built from: [map_build](../tools/map_build.html) reads an extract through it to
produce both the tile archive and the road graph. No Qt, no zenoh, no I/O beyond
a span of bytes, the same split as [mvt](mvt.html) and `gsof` and for the same
reason: everything here is reachable from a buffer, so all of it is testable
without a 637 MB file. The protobuf underneath is [protowire](protowire.html);
zlib is linked privately for inflate.

The split inside the library matters as much as the one around it. `blob.h`
walks the framing and yields still-compressed spans; `block.h` turns one such
span into entities as a pure function. That is what lets `map_build` inflate
and parse on every core while the framing walk stays sequential. Fusing them
into `for (block : file) visitor.onNode(...)` would bake in single-threaded,
and at continental scale that is the difference between a five-minute pass and
a twenty-minute one, per pass, and there are two.

## Public headers

| Header | Declares |
|---|---|
| `osm/blob.h` | `BlobIterator` over the framing, `Blob` (kind, type string, still-compressed span, file offset), `inflateBlob()` into a caller-owned buffer, and the spec's header and blob size caps. |
| `osm/block.h` | `Header` plus `decodeHeaderBlock()` and `checkRequiredFeatures()`; `Block` with flat string, tag, ref, member, node, way and relation arrays; `decodeDataBlock()`; `peekDataBlock()` and `BlockContents`. |
| `osm/entity.h` | `Coord` (1e-7 degrees), `kNoCoord`, `hasCoord()`, and the flat `Tag`, `Member`, `Node`, `Way`, `Relation` records that hold ranges into a block's shared arrays. |
| `osm/node_store.h` | `NodeStore`, the two-pass bitset-rank-array id to coordinate map, and `OrderCheck`, which verifies (type, id) ordering while streaming. |
| `osm/error.h` | `Error` with kinds `Truncated`, `Malformed`, `Unsupported`, `Decompress`, `OutOfOrder`, `Io`; `Result<T>`; `from_wire()` to rebase a `protowire::Error` offset onto the file. |

## Using it

Link the `osm` target. The header is read first, then every data pass is the
same loop: iterate blobs, skip anything that is not `OSMData`, inflate into a
reused buffer, decode. `tools/map_build/extract.cpp` runs that loop three times
over one mmap'd file.

```cpp
std::vector<std::uint8_t> buffer;
osm::BlobIterator it(file.bytes());
while (!it.done())
{
    auto blob = it.next();
    if (!blob) return std::unexpected(blob.error());
    if (blob->kind != osm::BlobKind::Data) continue;
    if (auto ok = osm::inflateBlob(*blob, buffer); !ok) return std::unexpected(ok.error());
    auto contents = osm::peekDataBlock(buffer, blob->offset);
    if (!contents->hasWays && !contents->hasRelations) continue;
    auto block = osm::decodeDataBlock(buffer, blob->offset);
    for (const osm::Way& way : block->ways()) { /* block->tags(way), block->refs(way) */ }
}
```

`decodeDataBlock()` touches no shared state, so a caller may hand each inflated
buffer to a pool and run one per core. `peekDataBlock()` parses group headers
only, which is how the first pass steps over the node blocks that are most of a
sorted file.

## Behaviour worth knowing

**Coordinates.** `Coord` is `int32` in 1e-7 degrees, converted once from PBF's
nanodegrees in `block.cpp`. A coordinate that was never written is `kNoCoord`
(`INT32_MIN`), not zero: zero is a real place in the Gulf of Guinea, and a road
drawn to it stretches across the world while a routing edge to it is 8000 km
long with a four-minute cost a router will choose for every long trip.
`NodeStore::get()` returns nothing for a node that was referenced but never
resolved, which is routine at an extract boundary; the caller must drop the
whole way.

**Five format traps that render rather than fail.** Coordinates are
`1e-9 * (offset + granularity * delta)`; `keys_vals` is one array with a zero
terminator per node, not a list per node; delta accumulators reset per group,
not per block; ids and deltas are zigzag; way refs are delta-coded too. Getting
any of them wrong gives a map that is off by 100x, nodes carrying their
neighbour's name, nodes drifting into the ocean partway through a file, refs
resolving to nothing, or every way collapsing to a point.

**Ordering is verified, not trusted.** `HeaderBlock.optional_features` may say
`Sort.Type_then_ID`, but it is advisory, nothing validates it, and sorted
extracts routinely omit it. `OrderCheck` costs one comparison per entity and
turns an unsorted file into `OutOfOrder` at the byte where order breaks rather
than a graph with holes discovered forty minutes later.

**Required features are refused, not guessed.** `checkRequiredFeatures()` is
separate from `decodeHeaderBlock()` so a tool can report what it found before
deciding. A full-history file is refused by name.

**Memory.** `inflateBlob()` reuses the caller's buffer because a continental
extract is over a million blocks of up to 32 MB. The node store is bitset, rank
index and a dense array of 8 bytes per referenced node, sized from the file and
never from a constant, because the id space grows by up to a billion a year and
a hardcoded ceiling presents as the newest edits missing.

{: .warning }
The node store must be resident. Continental US is about 2.2e9 referenced nodes,
roughly 20 GB including the bitset and rank, and pass B's lookups are random
across all of it; against an SSD-backed mmap that is days rather than minutes.
This is a workstation requirement, not a design flaw.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `osm_test_blob` | `osm unit` | Framing: the length prefix is big-endian, an absurd header length is capped, a truncated file is refused, raw and zlib blobs round-trip, the buffer is reused, an unimplemented codec is named rather than treated as empty, an unknown block type is skippable. |
| `osm_test_block` | `osm unit` | The block grammar and each of the five traps above; relations with roles and types; a tag index past the string table is refused; peeking; header features are read and an unknown required one, or a full-history file, is refused. `tests/pbf_builder.h` encodes longhand from the spec rather than through this library, so a test cannot agree with the bug it checks for. |
| `osm_test_node_store` | `osm unit` | Referenced nodes round-trip, an unresolved reference is absent rather than Null Island, id 0 and superblock boundaries, a sparse id space costs only its bitset, and the ordering check refuses a node after the ways or descending ids within a type. |
| `osm_test_real_extract` | `osm slow` (timeout 600 s) | Two full passes over a real extract written by someone else's encoder, cross-checked against the road the existing map stack draws near Irvine. |

The real-extract test reads `OSM_TEST_EXTRACT`, defaulting to
`/Users/ryan/Documents/map_data/socal-260813.osm.pbf`. When the file is absent
it logs `SKIPPED` and passes, since the file is not in the repository and a
fresh checkout must still pass; a file that is present and will not map is a
failure. It is excluded by `ctest -LE slow`.
