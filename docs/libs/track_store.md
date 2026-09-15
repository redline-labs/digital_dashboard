---
title: track_store
parent: Libraries
---

# track_store

## Overview

The race-track catalogue, which lives in extra tables inside the `.mbtiles`
beside the tiles. [mbtiles](mbtiles.html)`::Archive` reads exactly two tables,
`tiles` and `metadata`, read-only and with no `CREATE`, so extra tables are
invisible to it and the tile path is unaffected by their being there. That is
what makes one artifact possible: tracks need tiles for drawing and
full-resolution geometry with a centreline and a start/finish gate for
everything that comes later, and a sidecar file carrying the second can
disagree with the first about which build it came from, with no symptom until
a lap distance is measured against a centreline from another ingest run. One
file cannot disagree with itself.

No zenoh, no Qt, no network. [map_server](../nodes/map_server.html) puts the
answers on the bus, and `libs/dashboard_widgets/widgets/map` never links this
at all; it asks the server. Keeping SQLite out of the GUI is the same rule
`libs/mbtiles` states: Qt reaches SQLite through `Qt6::Sql` with its own copy,
and the two must never meet. There is no spdlog because a track without a
centreline is the normal case for a fifth of the corpus, and whether that is
worth a log line is the caller's decision.

## Public headers

| Header | Declares |
|---|---|
| `track_store/types.h` | `TrackRecord`, `Gate`, and the persisted enums `Quality`, `GateSource`, `GeometryKind`, with no SQLite in sight, so `map_server` can name a record without linking the storage. |
| `track_store/store.h` | `Error` and `Result<T>`, `Blob` (a typed geometry array), the read-only `Store`, and the append-only `Writer`. |

## Using it

Link the `track_store` target. `tools/map_build/tracks.cpp` writes the tiles
with `mbtiles::Writer` first, lets that object go out of scope, and only then
appends the catalogue under the same build id.

```cpp
auto writer = track_store::Writer::append(path, buildId);
if (!writer) return std::unexpected(writer.error());
for (const auto& t : tracks)
{
    track_store::TrackRecord record;
    record.id = t.id; record.name = t.name; record.circuit = t.circuit;
    /* bounds, lengths, quality, gate ... */
    if (auto ok = writer->put(record); !ok) return ok;
    writer->putGeometry(t.id, track_store::GeometryKind::OuterRing, t.outline);
    writer->putGeometry(t.id, track_store::GeometryKind::Centerline, line.coords);
}
return writer->finish();
```

On the read side `map_server`'s tracksets call `track_store::Store::open()`,
iterate `tracks()`, and fetch one track's `geometry(id, kind)` on request.

## Behaviour worth knowing

**The build id is the guard.** `build_id` is written into both the mbtiles
`metadata` table and the catalogue's own `track_meta`, and `Store::open()`
returns `BuildMismatch` when they differ. That is the state a half-finished or
half-copied build leaves behind, and it would otherwise open, draw, and be
wrong. `buildId()` travels with every reply built from the store, and
`TrackRecord::venueId` is explicitly not stable across rebuilds, so a client
may use it within one reply and must never persist it. `TrackRecord::id` is
the source file stem and is stable.

**Open fails when the file is absent.** SQLite's default would create an empty
database and serve nothing, which looks exactly like a correctly configured
server with no tracks in range. An ordinary basemap archive opens as
`NoCatalogue`, distinct from `NotReadable`, so a server can say so rather than
claim the file is broken.

**A missing centreline is an answer, not an error.** `geometry()` returns an
empty optional for a kind the track lacks. A track that failed the QA gate has
an outline and no centreline, and `Quality` says why.

**Loaded at open, blobs on disk.** The catalogue is under a megabyte for 994
tracks and is read whole, so several zenoh RX threads answer point lookups
without a connection pool; the geometry blobs stay on disk because the
outlines are 49 MB of source and one track's worth is all anybody asks for at
a time. Blobs are raw little-endian arrays behind a four-byte header with a
version byte, not capnp: capnp is the wire, and a format written and read by
one build has no schema-evolution story to buy.

{: .warning }
`mbtiles::Writer::finish()` commits but does not close; only its destructor
does. `track_store::Writer::append()` opens the same file for writing, so the
`mbtiles::Writer` must have gone out of scope first. Two write handles on one
SQLite file is how an archive that opens and is missing rows happens.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `track_store_test_store` | `track_store unit` | A catalogue is invisible to `mbtiles::Archive`; every `TrackRecord` field survives the round trip; geometry blobs round-trip exactly; a `build_id` mismatch is refused; an ordinary basemap reports `NoCatalogue`. |

The test writes a real archive with `mbtiles::Writer`, appends a catalogue,
and reads it back through both libraries. There is no fixture file and no
real-data dependency.
