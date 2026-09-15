---
title: road_graph
parent: Libraries
---

# road_graph

## Overview

The routable road network, on disk and mmap'd. Read side and build side are
the same library because they share the record layouts and nothing else: the
build side is used only by [map_build](../tools/map_build.html), which never
ships, and the read side by [map_server](../nodes/map_server.html) and
`nodes/map_match`, which do. No zenoh and no Qt. A graph is a file, and
everything about it is testable from one.

The classification a segment carries comes straight from
[map_rules](map_rules.html); the builder takes it rather than re-deriving it,
which is the anti-divergence rule expressed as a dependency. The design is
written up in [Map build](../design/map-build.html); the facts below come from
the headers.

## Public headers

| Header | Declares |
|---|---|
| `road_graph/format.h` | The six on-disk decisions, `SegmentId` and its packing, the `Section` table, `FileHeader`, `NodeRecord`, `SegmentRecord`, `EdgeRecord`, `WayIndexEntry`, `SegmentIdIndexEntry`, `TurnRestrictionRecord`, `RTreeNode`, the segment flags. |
| `road_graph/graph.h` | `Graph::open()`, the record spans, `edgesFrom()`, `geometryOf()`, `indexOf()`, `segmentsOfWay()`, `turnAllowed()`, the templated `queryBox()`, `nearest()` and the `Match` it returns. |
| `road_graph/builder.h` | `Builder` with `SegmentInput`, `RestrictionInput`, `add()`, `addRestriction()`, `write()`. |
| `road_graph/search.h` | `BoundedSearch` and the one-off `boundedDistance()`, `Route`, bidirectional A* `findRoute()`. |
| `road_graph/contraction.h` | `ContractionOptions`, `ContractionStats`, `buildOverlay()`. Workstation only. |
| `road_graph/overlay.h` | `Overlay::open()` validated against a `Graph`, `incomingEdges()`, `findRouteVia()`. |
| `road_graph/overlay_format.h` | `OverlayHeader`, `OverlayArc`, `OverlaySection`, `routingChecksum()`, and the argument for CH over MLD. |
| `road_graph/geometry.h` | Header-only `distanceM()`, `bearingDeg()`, `bearingDeltaDeg()`, `projectOnto()`, `hilbertOf()`, `fromDegrees()`/`toDegrees()`. |
| `road_graph/error.h` | `Error` with kinds `NotFound`, `NotReadable`, `NotAGraph`, `VersionMismatch`, `Malformed`, `InvalidArgument`; `Result<T>`. |

## Using it

Link the `road_graph` target. On the vehicle a graph is opened once and
queried from any thread; this is `nodes/map_server/graphs.cpp` and the nearest
handler in `services.cpp`, and `nodes/map_match` does the same with a
`BoundedSearch` kept per matcher.

```cpp
auto graph = road_graph::Graph::open(config.path);
if (!graph) { SPDLOG_ERROR("{}", road_graph::to_string(graph.error())); return; }
auto overlay = road_graph::Overlay::open(config.path + ".overlay", *graph); // optional

const auto lat = road_graph::fromDegrees(request.getLatitudeDeg());
const auto lon = road_graph::fromDegrees(request.getLongitudeDeg());
const auto matches = graph->nearest(lat, lon, radiusM, wanted, heading);
for (const road_graph::Match& m : matches)
{
    const road_graph::SegmentRecord& segment = graph->segments()[m.segment];
    /* segment.id, graph->nameOf(segment), m.offsetCm, m.bearingDeg */
}
auto route = overlay ? road_graph::findRouteVia(*graph, *overlay, from, to)
                     : road_graph::findRoute(*graph, from, to);
```

On the workstation `map_build graph` feeds `Builder::add()` and
`addRestriction()` from the extractor and calls `write(path, builtAtUnixS)`;
`map_build overlay` opens the result and calls `buildOverlay()`, writing
`<graph>.overlay` beside it.

## Behaviour worth knowing

**The six decisions in `format.h`.** A *segment* is undirected and owns the
name, class and posted limit; a *directed edge* owns cost, access and
direction, and both ids are `uint64`, so getting them backwards flickers the
road-name display on a U-turn silently. The published `SegmentId` is derived
from the OSM way id and the piece's ordinal within it (44 and 20 bits), never
an array index, because Hilbert ordering and every OSM refresh renumber the
arrays. The way index stays in the artifact because turn restrictions name
from-way, via and to-way and cannot be resolved to split segments without it.
Geometry is stored once, referenced by `(offset, count)`, with direction on the
edge. Nodes and segments are ordered along a Hilbert curve because OSM ids are
spatially random and an id-ordered continental graph faults on most of an A*
expansion. The section table is extensible, so later stages add sections keyed
by the same stable ids rather than rewriting what is there.

**mmap and const after open.** Nothing reads the file into memory; opening a
continental graph costs a few page faults, and a route pulls in the pages it
touches. Because the mapping is const, `map_server` answers on several zenoh
threads with no lock, and `map_match` can hold the same file at the same time.
The fastest free-flow speed is stored in the header for A*'s heuristic; a graph
built before that field falls back to one scan at open, never per query, since
the scan reads every 64-byte `SegmentRecord`.

**Refusals.** A wrong magic or `kFormatVersion` is `NotAGraph` or
`VersionMismatch`, not a reinterpretation; the artifact rebuilds offline in
minutes. `Overlay::open()` checks the graph's counts, build time and
`routingChecksum()` over the edges and the turn restrictions, and refuses a
mismatch, because an overlay from another graph would return fast, confident,
wrong routes. Two graphs differing only by one banned turn have byte-identical
edge arrays, which is why restrictions are in the checksum.

**No edge-expanded graph in the file.** Expansion would roughly triple an
887 MB SoCal artifact to store what a binary search derives. The router's state
is a directed edge and `turnAllowed()` is consulted per transition. The
contraction hierarchy does expand, in memory, at build time, and its expanded
node id is the road graph's edge index, so an unpacked shortcut is a list of
edges with no translation step.

**Search sizes.** `boundedDistance()` gives up once every frontier node is past
the limit, because the matcher asks it dozens of times per fix about candidates
metres apart. `findRoute()` is fine to a few hundred kilometres; measured on
SoCal, the worst case at 30 to 80 km was already 676 ms, which is what the
overlay exists to fix. `ContractionOptions::stopAtFraction` defaults to a
measured `0.95`: the densest few per cent of nodes take the build from minutes
to hours, so they are left as an uncontracted core searched directly.

{: .note }
`segmentsOfWay()` returns an `iota_view`, not a span. It used to fill a
`thread_local` vector, and a second call on the same thread invalidated the
first caller's result.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `road_graph_test_format` | `road_graph unit` | A graph round-trips; ids survive a rebuild from differently ordered input; a way's segments are contiguous and ordered; geometry is stored once with direction on the edge; oneway produces a single edge; an unroutable segment is stored but not connected; a wrong magic or version is refused; ids pack and unpack. |
| `road_graph_test_spatial` | `road_graph unit` | The nearest road is found; nothing in radius matches nothing; heading separates a road from the one beside it and is direction-agnostic; candidates come back nearest first; an unroutable segment is never a match; a bigger road wins a close call; box queries; a large generated graph is searchable. |
| `road_graph_test_routing` | `road_graph unit` | Route geometry runs the right way; a oneway, a turn restriction and an `only_*` restriction each force the longer way round; a U-turn is refused where there is a choice; disconnected pieces give no route; bounded distance gives up rather than expanding; the header speed, the old-graph fallback and a graph with no speeds; a reused `BoundedSearch` matches fresh ones. |
| `road_graph_test_contraction` | `road_graph unit` | Every pair costs exactly what plain A* found, fully contracted, with a banned turn, with half the graph left in the core, and with nothing contracted; an overlay from another graph is refused. Drift in the U-turn rule is cost-neutral for node-to-node routing and is deliberately not claimed. |

All four build their graphs in the test and need no data file.
