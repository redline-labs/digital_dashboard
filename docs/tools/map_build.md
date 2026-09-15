---
title: map_build
parent: Tools
redirect_from: /map_build.html
---

# map_build

## Overview

One tool reads an OSM PBF and writes everything the map stack needs: the vector
tiles the dashboard draws, the routable road graph, and the contraction
hierarchy that makes routing fast. A sixth verb reads a directory of race-track
GeoJSON instead and writes the tracks tileset; it is documented on its own page,
[tracks](tracks.html).

```
 region.osm.pbf ─► tools/map_build ─┬─► region.mbtiles        ─► nodes/map_server ─► libs/dashboard_widgets/widgets/map
                                    ├─► region.graph          ─► nodes/map_server ─► map/nearest, map/route
                                    └─► region.graph.overlay  ─┘                     nodes/map_match
```

`tools/map_build` is workstation only. It never runs on the vehicle, which is
why it lives in `tools/` rather than `nodes/`: it holds tens of gigabytes of
scratch state and links things that have no business in a vehicle image, and the
directory boundary is what keeps it out of a deployment manifest.

{: .note }
`nodes/` runs on the vehicle. `tools/` runs on the workstation and never ships.

The reasons it exists at all, when tilemaker already produced a working archive,
are on the design page [Map build](../design/map-build.html): what tilemaker's
output lacked, why one classification table decides both how a road is drawn and
how it is routed, the geometry traps in the tiler, and the routing measurements
behind the overlay. This page is how to run it and how to tell whether what it
wrote is right.

## Running it

The binary is `./build/tools/map_build/map_build`. Paths below use
`${REDLINE_DATA_DIR}` because that is what `configs/map_server.yaml` expects;
see [environment](../reference/environment.html) for where it resolves on a
desktop.

```bash
MB=./build/tools/map_build/map_build
D=${REDLINE_DATA_DIR}/maps

# What is in this file? Reads everything, writes nothing.
$MB verify  --input $D/socal-260813.osm.pbf

# The routable graph.
$MB graph   --input $D/socal-260813.osm.pbf --output $D/socal.graph

# The tiles.
$MB tile    --input $D/socal-260813.osm.pbf --output $D/socal.mbtiles --name socal

# The routing overlay. Optional: routing works without it, more slowly.
$MB overlay --graph $D/socal.graph

# How fast is routing, with and without the overlay?
$MB route   --graph $D/socal.graph
```

`verify`, `graph` and `tile` are independent of each other and each reads the
PBF from scratch. `overlay` needs the graph, and `route` needs the graph and
picks up `<graph>.overlay` beside it when one exists. Every verb takes `--quiet`
to suppress progress lines, and `--json` and `--debug` are global, so
`map_build --json verify ...` and `map_build verify --json ...` are the same
command.

| verb | options that matter |
|---|---|
| `verify` | `--input` |
| `graph` | `--input`, `--output`, `--built-at` (Unix seconds stamped into the header; `0` uses the wall clock) |
| `tile` | `--input`, `--output`, `--name` (tileset name written into the metadata, default `map`), `--min-zoom` (default `0`), `--max-zoom` (default `14`) |
| `overlay` | `--graph`, `--output` (default `<graph>.overlay`), `--stop-at` (fraction of nodes to contract, default `0.95`), `--witness-settle` (default `200`), `--witness-hops` (default `5`) |
| `route` | `--graph`, `--overlay` (default `<graph>.overlay` when present), `--samples` (pairs per distance band, default `12`), `--seed` (default `1`) |

Measured on the SoCal extract (a 637 MB PBF) on a workstation:

| verb | time | peak RSS | output |
|---|---|---|---|
| `verify` | 21 s | 11 GB | — |
| `graph` | 28 s | 11 GB | 888 MB, 5.0 M segments, 3.7 M junctions, 9.5 M edges |
| `tile` | 84 s | 12 GB | 532 MB, 11.7 M features, 58 301 tiles, z0–z14 |
| `overlay` | 7 min | 3.5 GB | 829 MB, 35 M shortcuts |

Memory is the binding resource, and the peak is in PBF extraction rather than in
tiling, so the number to size a machine by is the `verify` or `graph` peak. The
design page has the profile.

### Where the outputs go

`nodes/map_server` serves all three. A tileset and a graph are each named in its
config and the name is what clients ask for; renaming a file changes one line
there and nothing in any style or layout:

```yaml
tilesets:
  - name: socal
    path: ${REDLINE_DATA_DIR}/maps/socal.mbtiles
graphs:
  - name: socal
    path: ${REDLINE_DATA_DIR}/maps/socal.graph
```

The overlay is not configured anywhere. `map_server` looks for `<graph>.overlay`
beside the graph; absent, it uses the plain search and returns the same route
more slowly, and present, it is validated against the graph and refused if it
does not match. A graph entry may be absent altogether, in which case the
nearest and route services answer `noSuchGraph` and the tiles are unaffected.

`nodes/map_match` opens the graph directly rather than through `map_server`,
under `graph:` in `configs/map_match.yaml`. A matcher makes dozens of lookups
per fix at 10 Hz, and a read-only mmap is shareable, so both processes hold the
same file with no coordination.

```bash
./build/nodes/map_server/map_server --config configs/map_server.yaml --check
```

opens every archive, the graph and its overlay without touching zenoh, and is
the fastest way to find out whether a path is right.

## Diagnosing

`map_build verify` is the first thing to run on a PBF you have not seen before.
It prints node, way and relation counts, drawn and routable way counts, the node
store's referenced-against-resolved figures (the only place the
dangling-reference split is visible), per-class counts for render and route
classes, per-layer label counts, and how multipolygon relations, administrative
boundaries and turn restrictions fared. A label layer at zero is the symptom to
look for; see the table.

`map_build route` prints the graph's counts, whether an overlay was found, and
per distance band the median and worst query times for plain A\* and, when an
overlay is present, for the overlay. It routes every sampled pair both ways and
prints `WORST COST DIFFERENCE` if the overlay's answer is ever more expensive
than A\*'s. That line means the overlay is wrong; rebuild it.

| symptom | look at |
|---|---|
| map draws, no labels | the `place` layer, and `name:latin`; `labels.cpp` reads that field, not `name` |
| "no coverage" over an area that has data | one malformed tile; look in the dashboard's log for `polygon ring with 2 points` |
| tiles come from the wrong archive | two `map_server`s on one zenoh key. `pkill -f "map_server --config"` and start one |
| routing answers `noSuchGraph` | `map_server --check`; it opens the graph and its overlay too, not only the archives |
| routing is slow | `map_build route`; if there is no `overlay` line, there is no overlay |
| routes differ from A\* | `map_build route` prints `WORST COST DIFFERENCE`; rebuild the overlay |
| a whole layer is empty | `map_build verify` prints per-layer label counts. A layer at zero usually means its tags are not in `map_rules::hasLabelTags()` |
| a shape is drawn twice, in two colours | it classifies into two layers. `classifyPark` and `areaRule` are the two that overlap |

The sixteen layers, in the OpenMapTiles vocabulary the widget's tessellator and
`labels.cpp` already read, are what `verify`'s label counts and the diagnostics
above refer to:

| | |
|---|---|
| shapes | `transportation` `building` `water` `waterway` `landuse` `landcover` `park` `aeroway` `boundary` |
| labels | `place` `poi` `housenumber` `transportation_name` `water_name` `mountain_peak` `aerodrome_label` |

How each was counted against tilemaker's archive, and the three silent rules that
accounted for the differences, are on the design page.

## Known gaps

These are the ones a person looking at the output will meet. Each is explained,
with what fixing it would take, on the design page.

Coastlines mapped as `natural=coastline` ways rather than as water polygons are
missing at low zoom, because low-zoom water comes from the PBF's multipolygon
relations alone rather than from preprocessed coastline shapefiles.

A label point is the area-weighted centroid, so on a strongly concave shape such
as a crescent reservoir the label sits on the land inside the crescent. It
affects a few hundred features out of a hundred thousand.

The tiler holds the whole pyramid in RAM, which is what stops a continental
extract as written. SoCal is fine.

`aeroway` and `landcover` sit at slightly wider zoom ranges than tilemaker's, and
z9 and z10 tiles are about 3x larger because more road classes are carried
there. Not a correctness problem; the dial is `map_rules`' per-class `minZoom`.
