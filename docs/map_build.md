---
title: Building the map
---

# Building the map

One tool reads an OSM PBF and writes everything the map stack needs: the vector
tiles the dashboard draws, the routable road graph, and the contraction
hierarchy that makes routing fast.

```
 region.osm.pbf ─► tools/map_build ─┬─► region.mbtiles        ─► nodes/map_server ─► dashboard/widgets/map
                                    ├─► region.graph          ─► nodes/map_server ─► map/nearest, map/route
                                    └─► region.graph.overlay  ─┘                     nodes/map_match
```

`tools/map_build` is **workstation only**. It never runs on the vehicle, which is
why it lives in `tools/` rather than `nodes/`: it holds tens of gigabytes of
scratch state and links things that have no business in a vehicle image, and the
directory boundary is what keeps it out of a deployment manifest.

> `nodes/` runs **on the vehicle**. `tools/` runs **on the workstation** and never
> ships.

## Why this replaced tilemaker

The archive `tilemaker` produced is a fine picture and nothing else. Checked
against the real file, its `transportation` layer carries `access, bicycle,
brunnel, class, expressway, foot, horse, service, subclass, surface, toll` — and
**no `osm_id`, no `oneway`, no `maxspeed`**. Geometry is clipped per tile,
quantised to about half a metre at z14, and simplified before that. Topology is
gone: nothing distinguishes a junction from an overpass.

So map matching, speed limits and routing could never have been served from it,
and the extractor had to be ours regardless. Given that, the tiler came too — so
that **one `map_rules::classify()` call decides both the drawn road and the
routable road**. Two rule sets eventually disagree, and the symptom is *"the
route goes down a road that isn't drawn on the map"*, which costs a day before
anyone suspects the cause.

## Running it

```bash
cd ~/Documents/map_data

# What is in this file? Reads everything, writes nothing.
map_build verify  --input socal-260813.osm.pbf

# The routable graph.
map_build graph   --input socal-260813.osm.pbf --output socal.graph

# The tiles.
map_build tile    --input socal-260813.osm.pbf --output socal.mbtiles --name socal

# The routing overlay. Optional -- routing works without it, more slowly.
map_build overlay --graph socal.graph

# How fast is routing, with and without the overlay?
map_build route   --graph socal.graph
```

Real numbers for the SoCal extract (637 MB PBF) on a workstation:

| verb | time | peak RSS | output |
|---|---|---|---|
| `verify` | 21 s | 11 GB | — |
| `graph` | 28 s | 11 GB | 888 MB, 5.0 M segments, 3.7 M junctions, 9.5 M edges |
| `tile` | 84 s | 12 GB | 532 MB, 11.7 M features, 58 301 tiles, z0–z14 |
| `overlay` | 7 min | 3.5 GB | 829 MB, 35 M shortcuts |

**Memory is the binding resource, and the peak is in extraction rather than in
tiling.** Sampled through a `tile` run, RSS climbs to 10.8 GB at 22 s, drops to
5.1 GB the moment the node store is released, and then plateaus at 7.7 GB for
the whole pyramid. So the number to size a machine by is the extract peak: the
node store (bitset, rank index and coordinate array) and the accumulating
feature list are alive at the same time, and they have to be — a feature cannot
be built without the coordinates it references.

## The three geometry traps in the tiler

Each of these renders. None of them fails.

**A degenerate ring poisons the whole tile.** A pond quantised at low zoom
becomes the ring `[A, B, A]`. `ClosePath` implies the closing point, so it goes
out as two points, and a two-point ring makes the **tile** malformed — not the
feature. A decoder that rejects a malformed tile discards every road, label and
coastline in that square, and the map reports "no coverage" over a tileset that
has the data. The guard lives in `mvt::encode`, so nothing can encode past it.

**Generalization, not simplification, is what makes low-zoom tiles small.**
Douglas–Peucker thins a shape's points; it never removes the shape. Without a
minimum-extent rule, every cul-de-sac and back garden in the region is carried at
every zoom — an eight-fold size increase at z9 for detail occupying well under a
pixel.

**A line that leaves a tile and comes back is two parts.** Joined, the renderer
draws a straight line across the tile between the two crossings: a road that
visibly cuts a corner it never had.

## Identity, and where it stops

Every drawn feature carries its **source OSM way id**, which is what lets a
client recolour the road it is on, or draw a route by highlighting features it
already has, rather than overlaying a full-precision polyline that visibly
diverges from the simplified tile geometry at low zoom.

Below z13, features that share every attribute are **merged**, and the merged
feature loses its id — because it is "Jamboree Road" and not any one way, and
keeping the first id would silently attribute the whole road to an arbitrary one
of them. That boundary is a contract, not a preference: at and above z13 identity
survives, and that is where a client joins back to the graph.

## Routing, and why the overlay is contraction hierarchies

`map_build route` samples origin/destination pairs by distance band and times
them. On the SoCal graph, plain bidirectional A\*:

| straight line | median | worst |
|---|---|---|
| 2–10 km | 17 ms | 66 ms |
| 10–30 km | 67 ms | 138 ms |
| 30–80 km | 152 ms | 677 ms |
| 80–200 km | 495 ms | 641 ms |

The cost grows with roughly the square of the distance, because A\* expands an
ellipse and the ellipse's area does. Extrapolated to a continental trip that is
minutes per query. The worst case is already 677 ms at 30–80 km, and the number
that matters in a vehicle is the query that makes someone wait.

**CH, not MLD.** MLD's advantage is cheap re-customization when weights change;
there is no live traffic here and the build is offline, so that advantage is
bought and never used. MLD's other advantage is one partition shared across
profiles, where CH needs an independent contraction per profile — but that is a
disk cost, and disk is the resource this project has in surplus. What is left is
query speed, and CH wins it.

**The hierarchy is contracted over the edge-expanded graph**, where a node is one
directed edge and an arc is one legal transition. Turn restrictions are
properties of a transition and cannot be expressed on a node graph at all. A
useful consequence: an expanded node id and a road-graph edge index are the same
number, so an unpacked shortcut falls out as a list of edges with no translation
step — and a translation step is exactly where a routing bug hides.

**The last few per cent are left uncontracted.** A road network's core is dense,
and contracting it is where a build goes from minutes to hours. Everything above
`--stop-at` keeps one shared rank, and the query searches it directly: a plain
bidirectional Dijkstra over a few hundred thousand nodes instead of nine million.

With the overlay:

| straight line | A\* median | overlay median | A\* worst | overlay worst |
|---|---|---|---|---|
| 0–2 km | 16 ms | 0.4 ms | 16 ms | 0.4 ms |
| 2–10 km | 17 ms | 3.6 ms | 66 ms | 25 ms |
| 10–30 km | 67 ms | 19 ms | 138 ms | 46 ms |
| 30–80 km | 152 ms | 81 ms | 677 ms | 100 ms |
| 80–200 km | 495 ms | 163 ms | 641 ms | 169 ms |

The worst case is what moved most: 677 ms to 100 ms.

## The overlay is optional, and checked

`nodes/map_server` looks for `<graph>.overlay` beside the graph. Absent, it uses
the plain search and returns the same route more slowly, so a vehicle with no
overlay is slower and never wrong.

Present, it is **validated against the graph** — build time, counts, and a
checksum over the edges *and the turn restrictions*. That last part is not
theoretical: two graphs differing only by one banned turn have byte-identical
edge arrays, and a hierarchy built for one of them turns through the ban in the
other. A mismatch is refused, loudly, because an overlay pointed at the wrong
graph does not crash — its shortcuts name edge indices that still exist and now
mean other roads, and the router answers quickly, confidently, and wrongly.

## How the hierarchy is trusted

`road_graph_test_contraction` routes **every pair** of junctions in a small grid
both ways and requires the costs to be identical, with a banned turn, with the
graph fully contracted, with half of it left in the core, and with nothing
contracted at all. `map_build route` does the same on the real graph for every
sampled pair and prints a loud line if any cost differs.

That is the only honest way to test a hierarchy. A wrong one does not crash and
does not return nothing — it returns a route, quickly, that is not the shortest,
and nobody notices until someone drives it.

## Diagnosing

| symptom | look at |
|---|---|
| map draws, no labels | the `place` layer, and `name:latin` — `labels.cpp` reads that field, not `name` |
| "no coverage" over an area that has data | one malformed tile; check `app_logs` for `polygon ring with 2 points` |
| tiles come from the wrong archive | two `map_server`s on one zenoh key. `pkill -f "map_server --config"` and start one |
| routing answers `noSuchGraph` | `map_server --check` — it opens the graph and its overlay too, not just the archives |
| routing is slow | `map_build route`; if there is no overlay line, there is no overlay |
| routes differ from A\* | `map_build route` prints `WORST COST DIFFERENCE`; rebuild the overlay |
| a whole layer is empty | `map_build verify` prints per-layer label counts. A layer at zero usually means its tags are not in `map_rules::hasLabelTags()` |
| a shape is drawn twice, in two colours | it classifies into two layers. `classifyPark` and `areaRule` are the two that overlap |

## The layers, and how they were checked

Sixteen layers, in the OpenMapTiles vocabulary the widget's tessellator and
`labels.cpp` already read:

| | |
|---|---|
| shapes | `transportation` `building` `water` `waterway` `landuse` `landcover` `park` `aeroway` `boundary` |
| labels | `place` `poi` `housenumber` `transportation_name` `water_name` `mountain_peak` `aerodrome_label` |

Every one was counted against the archive tilemaker produced from the same PBF,
at z14 — the only honest zoom, because it is the only one where neither build
simplifies or merges. Roads, buildings, water, landcover and waterways all land
within a couple of percent. The gaps that remain are all explained, and every
one of them is us being *more* complete:

| layer | tilemaker | ours | why |
|---|---|---|---|
| `housenumber` | 1 027 343 | 2 143 892 | tilemaker writes NO feature id on these and collapses duplicates: one feature per distinct number per tile. Two houses at number 100 on different streets become one. Ours keeps both. |
| `mountain_peak` | 2 487 | 1 794 | tilemaker writes a label point into every tile whose *buffer* it touches, so a summit near an edge is counted several times. By DISTINCT peak we carry more: 1 563 against 1 405. The same applies to `place` and `aerodrome_label`. |
| `park` | 5 012 | 16 574 | national parks and preserves are `type=boundary` relations. tilemaker assembles only `type=multipolygon`, so Joshua Tree, Mojave and Anza-Borrego are simply absent from its archive. |
| `poi`, `landuse` | — | +13%, +6% | our allow-lists are wider; the value passes through as `subclass` rather than being dropped. |

Three rules account for nearly all of the difference that was NOT us being more
complete, and all three were silent:

**Water is tested before landuse and leisure.** A swimming pool is
`leisure=swimming_pool` and a settling pond is `landuse=basin`. A rule that
reaches either key first files both as ground cover — and Southern California
has hundreds of thousands of pools. This one rule moved `water` from 23 114
features to 77 736 against tilemaker's 78 090.

**A way earns its coordinates by carrying a LABEL, not only by being drawn or
routable.** A runway is `aeroway=runway` and nothing else. Discarding it before
the label rules run costs the entire `aeroway` layer: it produced 47 features
where tilemaker had 11 172. `map_rules::hasLabelTags()` is that gate, and
`map_rules_test_labels` checks it agrees with every classifier — because a
classifier that reads a key the gate does not list keeps working perfectly for
nodes and silently stops being asked about ways.

**A shape belongs to one layer.** A nature reserve is a designation over terrain
that is forest, rock and water at once; filling it green says something about
the ground that is not true, as well as drawing the same outline twice. US
National Forests are excluded outright, as tilemaker excludes them — they cover
most of the mountains behind Los Angeles and wash out the roads underneath.

## Known gaps

**Low-zoom water comes from OSM alone.** OpenMapTiles builds z0–8 water from
preprocessed coastline shapefiles rather than from the PBF; we assemble
multipolygon relations instead. Coastlines mapped as `natural=coastline` ways
rather than as water polygons are therefore missing at low zoom.

**A label point is the area-weighted centroid**, not the pole of inaccessibility.
For a strongly concave shape — a crescent-shaped reservoir — the centroid falls
on the land inside the crescent and the label sits outside its own lake. It
matters for a few hundred features out of a hundred thousand; worth fixing when
something renders these labels and the misplacement is visible.

**The tiler holds the whole pyramid in RAM**, and that is what stops this
reaching continental scale as written. Features are walked feature-major — for
each feature, find the tiles it touches — so every tile of a zoom is open at
once. At SoCal's 41 818 z14 tiles that costs about 2.6 GB and is fine; at
continental it is not, and neither is holding 11.7 M features. The fix is not
subtle, only tedious: bucket features to tiles on disk first, then process
tile-major so one band of rows is resident at a time. Nothing in the format or
the rules has to change for it.

**`aeroway` and `landcover` sit at slightly wider zoom ranges than tilemaker's**,
and low-zoom tiles are larger, about 3x at z9 and z10, because more road classes
are carried there. Not a correctness problem; the dial is `map_rules`' per-class
`minZoom`.
