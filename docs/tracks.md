# Race tracks

994 tracks, worldwide — circuits and point-to-point courses — in a 42 MB file
that is rebuilt independently of the basemap.

```bash
./build/tools/map_build/map_build tracks \
    --input /Users/ryan/Documents/map_data/tracks \
    --output /Users/ryan/Documents/map_data/tracks.mbtiles \
    --report /tmp/tracks_qa.csv
```

That takes about a second and touches no PBF. A new drop of track maps does not
rebuild the 383 MB SoCal basemap, and a new OSM extract does not touch the
tracks — which is the whole reason the layer is separate.

## What the source data actually is

Not what it looks like. A file named `Monza.geojson` holds a single ring whose
traced length is 11 530 m against a published lap of 5 762 — **exactly twice**.
It is not a centreline and not a racing line: it is the edge of the tarmac,
traced all the way round the outside and then all the way back round the inside,
joined into one ring.

Each file is a `FeatureCollection` of two features:

| feature | carries |
|---|---|
| `Polygon` (994) | the outline, plus `name`, `closed`, `degenerate` |
| `Point` named `Start / Finish` (992) | `circuit`, **`length_m`** (published lap length), `gatewidth_m`, `combo` |

`length_m` is the only independent measurement of a track anywhere in the
pipeline, and the whole QA gate is built on it.

### Four things that bite

**The ring has no hole, and its winding means nothing.** Every closed file has
exactly one entry in `coordinates`, and the two loops are wound the same way as
often as not — Monza and the Nordschleife agree, Laguna Seca disagrees. Hand the
raw ring to a tessellator as one polygon and it fills the infield solid: a blob
where the track should be, with nothing logged and nothing thrown. The ingest
splits it at the seam and passes the two loops on as **roles** (outer, inner), so
the tiler cuts the infield as a hole.

**A file can hold several loops.** Charlotte's oval plus its infield, and the
`*_Combo` layouts. Split one of those at any single loop boundary and you get a
centreline of entirely plausible width and twice the correct length.

**The two loops are sampled at different densities.** Adria's inner loop carries
four times the vertices of its outer, so pairing by vertex index compares a
hairpin against a straight. The pairing is by arc length.

**Every outline is closed, point-to-point courses included.** There are no open
outlines anywhere: across all 994 files the gap between the last vertex and the
first is at most 3.4% of the perimeter, 0.2% at the median. Pikes Peak is not an
open line — it is a thin closed ribbon whose 39 745 m perimeter is twice its
19 915 m course, exactly as a circuit's is.

So **nothing about the boundary says whether a track is a circuit**, and neither
does the feature type or the `closed` property. Only the derived centreline can.
Routing that decision off the geometry type would misclassify every hillclimb.

## Deriving the centreline

Two topologies, because the two kinds of course differ in how their boundary is
built:

**Annulus — a circuit.** The surface is a ring, so the boundary is two closed
curves that the trace joins by returning to where it started. Split at that
seam, resample both loops by arc length, search {direction} x {phase} for the
alignment minimising mean pair distance, average the aligned pairs. Gives a
**closed** centreline.

**Ribbon — a point-to-point course.** The surface is long and thin, so the
boundary is one closed curve with a fold at each end. There is no seam. The
pairing is *antipodal* instead: walking out from a fold at sample `r`, sample
`r+i` is on one edge and `r-i` is on the other, directly across the track.
Finding the fold is a search over `r`, the same coarse-then-fine shape as the
circuit's phase search. Gives an **open** centreline.

Both are tried and the better result wins, because the topology is not knowable
in advance. The distance between each pair **is** the local track width.

### Which one it was

From the centreline that came out: endpoint gap over its length.

| | endpoint gap / length |
|---|---|
| circuits | 0.0015–0.015 (Monza 0.005, Nordschleife 0.0015) |
| point-to-point | 0.15–0.85 (Aintree Sprint 0.15, Gurston Down 0.85) |

Two orders of magnitude apart with nothing in between, so the 0.05 threshold is
not a tuned number. `Centerline::closed` is the only thing in the pipeline that
answers this question, and `TrackRecord::closed` carries it through.

An open centreline's **direction is arbitrary** — the two folds are
interchangeable — so nothing may assume index zero is the start of the course
rather than the finish.

`tools/map_build/track_geometry.h` — no sqlite, no capnp, no GeoJSON, no
mbtiles, so the awkward cases are built by hand in `tests/test_track_geometry.cpp`
against an annulus, whose answers are known in closed form.

### The QA gate is two checks, and both are internal to the geometry

1. median pair width in `[3, 30]` m. Circuits sit at 12–20 m; point-to-point
   courses are public roads and genuinely narrower, so the floor has to reach
   down to Gurston Down's 4.2 m and Osnabrück's 5.5 m;
2. derived length within 5% of **half the outline's own length**.

Neither consults `length_m`, and that is deliberate. The outline is a KML
digitisation; `length_m` is the `length` attribute of a separate Racelogic
StartFinish catalogue. They are independent sources, so a disagreement says one
of the two is wrong — and on 40 tracks it is the label, not the geometry: Inde
Motorsports Ranch Full's published figure is a third of its outline, Desert North
Palm's about half. Gating on it threw those away to protect a number nothing
measures against, since distance-along-lap comes from the geometry.

So the cross-check against `length_m` is kept as a **warning**
(`publishedLengthDisagrees`, `published_disagrees` in the report) rather than a
veto. 36 accepted tracks carry it.

What the published figure used to catch — a file holding several loops, where
the derivation is self-consistently wrong by a factor of two, as
`Monza_without_Chicane` is — is caught independently by the seam's revisit count.
That is what makes dropping it safe.

The source's own `degenerate` flag is honoured too. One file carries it: 329
points on a flat line at constant latitude in the Atlantic.

**Repeated vertices do not need deduping.** 87 of the outlines carry runs of
identical consecutive vertices — Milford Road Course has 2 346 — and a revisit
count that treats each as its own return reads a plain circuit as holding ten
loops. The excursion scan takes the minimum over a run of near approaches and
skips past it, so a stutter collapses to one candidate however long it is. Arc
length is unaffected either way, since a zero-length segment adds nothing.

Current results over all 994:

| verdict | count | |
|---|---|---|
| `ok` | 842 | 827 annulus + 15 ribbon; median length error 0.64%, p95 2.7% |
| `width-out-of-range` | 108 | figure-8 crossovers (Suzuka), odd geometry |
| `length-mismatch` | 20 | the centreline disagrees with the outline it came from |
| `multiple-loops` | 23 | ovals + infields, `*_Combo` layouts |
| `degenerate` | 1 | source-flagged |

Spot checks against published lengths: Monza 5751 vs 5762, the Nordschleife
20710 vs 20741, Spa 6970 vs 6994, Laguna Seca 3584 vs 3600, and among the
point-to-point courses Pikes Peak 19 698 vs 19 915, Osnabrück 2311 vs 2318.

**A rejected track still renders.** The outline is what draws and it is fine
whatever happened to the centreline. `quality` says why there is no lap distance,
rather than leaving the absence to be guessed at.

### The start/finish gate

The `Point` is projected onto the centreline and a perpendicular is cast to each
boundary.

On a **closed** centreline the lap has no natural beginning, so the centreline is
rotated to put the gate at sample zero and the distances run forward from it. On
an **open** one the course already has a beginning and rotating an open polyline
does not renumber it — it tears it in half and joins the two ends across the map,
with the right point count and the right distances and entirely the wrong shape.
So the distances are left running from one end and `centerlineOffsetCm` records
how far along the gate falls.

The on-track test is **distance from the centreline against the local half
width**, not a crossing test against the two loops. The crossing test is the
obvious implementation and it is wrong here: these points sit essentially ON the
outline, 1–10 m from the nearest boundary vertex and usually equidistant from
two of them, i.e. on an edge. Even-odd parity for a point on an edge is a coin
flip, and it rejected 310 of 844 perfectly good gates.

Worth knowing for whatever eventually asks "am I on this track", because the
same trap is waiting there: a point on the racing surface is inside the outer
loop and **outside** the inner one, while a point in the infield is inside
*both*. A single point-in-polygon call against the outline answers the wrong
question, and against an edge it answers it at random.

962 of the 992 files with a point get a gate.

## One artifact, two readers

An mbtiles archive is a SQLite database, and `mbtiles::Archive` reads exactly two
tables out of it with `SQLITE_OPEN_READONLY` and no `CREATE`. Extra tables are
invisible to it. So the catalogue lives **inside** the `.mbtiles`, in
`track_catalog`, `track_geometry` and `track_meta` (`libs/track_store`).

A sidecar file beside the archive was the obvious alternative and is the wrong
one: two files that can disagree about which build they came from, with no
symptom until something measures a lap against a centreline from a different
ingest run. One file cannot disagree with itself.

The guard that makes that checkable is `build_id`, written into **both** the
mbtiles `metadata` table and `track_meta`, and refused at open when the two
differ — which is the state a half-finished or half-copied build leaves behind.

Ordering matters and is easy to get wrong: `mbtiles::Writer::finish()` commits
but does **not** close, only its destructor does, so `track_store::Writer::append`
must run after that object goes out of scope.

## Serving it

```yaml
tilesets:
  - name: socal
    path: .../socal.mbtiles
  - name: tracks
    path: .../tracks.mbtiles
tracksets:
  - name: tracks
    path: .../tracks.mbtiles
```

The same path appears twice on purpose: `mbtiles::Archive` reads the tiles,
`track_store::Store` reads the catalogue. Different libraries, different
questions, and either half can be present without the other — an ordinary
basemap has no catalogue at all. Do not tidy it into one entry.

```
map_server --config configs/map_server.yaml --check
```
reports the trackset with its build id and counts by quality.

Two query services beyond the tiles, on `schemas/map_tracks.capnp`:

| key | answers |
|---|---|
| `map/track_catalog` | what tracks exist, where, and their quality. Filterable by venue or by a point and radius |
| `map/track_detail` | one track's full-resolution outline, centreline, per-point distance and half width, and its gate |

Tiles are lossy by design — simplified per zoom, clipped to a tile. The detail
service exists because none of what a lap needs survives that, and reassembling a
circuit from four clipped tiles to measure against it is not a thing anybody
should attempt.

`centerlineDistanceCm` + `gate.centerlineOffsetCm` are what any future
distance-along-lap consumer needs, indexed by **distance rather than time** —
which is what makes one lap comparable against another driven at a different
speed. Nothing consumes them yet; they are on the wire so that whatever
eventually does is not blocked behind a re-ingest.

## Drawing it

```yaml
config:
  tileset: "socal"
  overlay_tilesets: ["tracks"]
```

`MapWidget` holds one `TileSource` per archive, each learning its own zoom range
from its own server replies and keeping its own cache and backoff. That is what
makes "no coverage here" and "that archive is missing" distinguishable — and it
costs one extra query batch per viewport, which is nothing for a layer where
nearly every tile comes back `notFound` and absence is cached permanently.

Three layers: `track` (the surface, a fill with the infield as a hole for a
circuit and solid for a point-to-point course, which has no infield, z11+),
`track_centerline` (z12+), `track_label` (z8+, gathered alongside the basemap's
`place` labels and placed by the same collision pass).

> **Naming.** `MapConfig_t` already uses `track_*` for the vehicle's own
> breadcrumb trail — `show_track`, `track_points`, `track_width`. Style fields
> for circuits are `racetrack_*`.

## Checking it looks right

```
app_launch(app="map_server", config="configs/map_server.yaml")
app_launch(app="dashboard", config="configs/dashboard/map_demo.yaml")
```
then point the map at Willow Springs (34.87255, -118.2645) at z14. Buttonwillow,
Willow Springs and Streets of Willow are all inside the SoCal basemap, so
basemap and circuit coexist there.

**Check the infield is not filled.** That is the winding trap, and it is a
visual failure with no error behind it. Setting `racetrack_surface` to something
bright makes it unmistakable: the surface should be a ribbon with the basemap's
roads visible straight through the middle.

## Known gaps

- **152 tracks have no centreline** and so no lap distance. They render. DTW
  refinement after the rigid phase search would recover the figure-8 crossovers;
  `--report` is the metric to measure that against.
- **Source-data problems the report surfaces**, worth fixing upstream rather than
  in code: `Inde_Motorsports_Ranch_Full`'s outline is 3.00x its published length;
  34 files (mostly `*_Combo`) have no `length_m`, which skips the strongest QA
  check; `gatewidth_m` is a placeholder 25.0 on 970 of 992.
- **Accepted centrelines run ~0.6% short** of `length_m`, consistently. Confirmed
  as a source difference rather than an error in the pairing: 927 of the outlines
  are KML digitisations, straight-chorded between vertices, while `length_m`
  follows the true curve from a separate catalogue. Closing it would need a
  spline resample, and there is no smoother source in the data.
- **23 outlines are corrupt upstream, and are not drawn.** They hold *other*
  layouts at the same venue concatenated, and never the layout they are named
  after: `Buenos_Aires_F` is every Buenos Aires layout except F,
  `Road_Atlanta` is the Short and Combo layouts, `Monza_without_Chicane` is
  Monza and the Combo. That self-exclusion is the diagnostic — a deliberate
  "all layouts here" export would include its own.

  Traced to the source vectors rather than to any step in this pipeline. The
  named layout survives as a raster for 21 of the 23 and does not exist at all
  for two (`Buenos_Aires_F`, `estering_Combo`), so there is no clean vector to
  fall back to.

  These are the one case where a rejected track is **not** rendered. Everywhere
  else the outline is sound and only the centreline failed; here the outline is
  not the track it names, so drawing it would overdraw two real siblings and
  hang the wrong name on them. The catalogue row is kept with its verdict, so
  the track is diagnosable rather than silently absent.

  21 of the 23 venues keep at least one usable layout. Two do not:
  `Circuito_De_Rosario` and `estering`. In four cases the corrupt file is the
  venue's *longest* layout, so what is lost is the headline course rather than a
  variant: Road Atlanta (4050 m, next best 2825 m), Miller Motorsports Park
  Full, Bruntingthorpe Full, Heartland Park Topeka D.

  **`estering` is unrecoverable by anyone.** It is one of the two entries with no
  on-device raster and no catalogued length either, so the named layout's real
  shape exists nowhere in the source — not as a vector, not as an image. Nothing
  downstream can bring it back, and it should not be chased.
- **7 start/finish points land off the traced tarmac.** Same two-source problem:
  the gate is Racelogic's coordinate, the outline is the KML. Castle Combe
  Western Sprint is 23 m out, Anneau du Rhin 3.0 km 16 m. Not fixable here.
