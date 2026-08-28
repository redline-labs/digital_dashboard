---
title: Offline maps
---

# Offline maps

A map on the dash, from a file, with no internet anywhere in the path.

```
 socal.mbtiles ─► libs/mbtiles ─► nodes/map_server ─► zenoh ─► dashboard/widgets/map
                  (format only)    map/tile                    TileSource ─► libs/mvt ─► tessellator
                                   map/catalog                       │                        │
                                   map/asset                         │                   GpuRenderer (QRhi)
                                                                     └────────────────►  labels (QPainter)
```

Three pieces. `libs/mbtiles` reads the archive. `nodes/map_server` answers zenoh
queries for tiles, catalogs and assets. `dashboard/widgets/map` fetches tiles,
decodes them with `libs/mvt`, turns them into triangles, and draws those on the
GPU.

## Why the renderer is ours

An off-the-shelf Qt map widget was the obvious choice, and one was built, wired
up and then removed. Two findings killed it:

- **They are `QRhiWidget` or `QOpenGLWidget` derived**, and under
  `QT_QPA_PLATFORM=offscreen` such a widget reports *"QRhi is not supported on
  this platform"* and hands back a **null** map. Every `gui` test and every
  `ui_screenshot` in this tree runs offscreen, so the map would have been
  invisible to the entire agent-control loop — and the first symptom was a
  segfault, not a blank image.
- It cost **7 GB of recursive submodules and 42 minutes of configure** to find
  that out, on every fresh checkout.

A vector tile is protobuf with a command-encoded geometry stream. That turned
out to be a smaller thing to own than the consequences of not owning it. What
the replacement gives up: **no 3D** — see below — and no GL style documents.

What it gains, beyond working offscreen: **no glyph PBFs**. `QPainter` has
fonts, so there is no font pipeline, no sprite sheet, and no "the map has no
labels and nothing said why" failure mode.

## What it does not do: 3D

The renderer is **2D only**, by construction rather than by omission:

- `MapVertex` carries `x, y` and a 2D normal, and the triangles that use it are
  an **index buffer** — a polyline is two vertices per point rather than six per
  segment, which measured 2.24x smaller on a road-and-lake mix. There is no z,
  no depth buffer,
  and no depth test — correctness comes from painter's-algorithm draw order
  (layer-major across tiles), which works because everything is coplanar. Note
  *coplanar*, not *axis-aligned*: tilting the camera does not break it.
- `Camera` has `center`, `zoom` and `bearing`. There is **no pitch**, so the
  view is always straight down; the MVP is an orthographic projection with a
  rotation about z.
- Buildings are drawn as **footprints**, not extrusions. The data is there and
  unused: the `building` layer in the bench archive carries `render_height` and
  `render_min_height`, and the tessellator reads neither.

There is no plan for either: a dash map is read at a glance, and a tilted view
trades legibility for looks. But they are **two separate projects of very
different size**, and it is worth not conflating them.

**A tilted camera (pitch) does not need a depth buffer.** Everything drawn is
coplanar on the ground, so layer-major painter's order stays correct at any
camera angle, and `map.vert` is already projective — `gl_Position = mvp *
vec4(p, 0, 1)` with a full `mat4`, where today `w` is always 1. The per-tile
uniform block survives untouched: it is a model matrix placing a unit quad on
the ground, times a shared view-projection, and only the shared half changes.

What pitch actually costs, in order:

1. **Per-tile LOD — this is the project.** `refreshTiles()` picks one integer
   zoom for the whole frame and `tileBounds()` takes the axis-aligned box of
   four projected screen corners. Under pitch the visible region is a trapezoid
   running to the horizon, and the top corners have no ground intersection at
   all. Uniform z14 to the horizon is hundreds to thousands of tiles against a
   hard `kMaxTilesPerFrame`. It needs a quadtree descent emitting leaves, plus a
   far-plane cutoff and a pitch cap. Everything downstream already copes:
   `TileSource` is keyed by `TileId` *across* zooms, so mixed-zoom tile sets are
   already representable end to end.
2. **Line widths.** `map.vert` expands lines in tile-local space by
   `halfPx * widthScale / pxPerLocal`, one constant per tile. Under perspective
   the screen scale varies *across* a tile, so distant roads shrink to
   sub-pixel. The fix is to expand in clip space after projection, which wants
   the viewport size as one more uniform — worth doing regardless, since it
   decouples road width from tile size.
3. **Labels.** `Projection::tileTransform()` is a 2D similarity — a scale, a
   rotation and an offset. Under perspective each point has to go through the
   full matrix instead; the transform object is already the seam for that, so
   it is one implementation rather than one per call site. The new problem is
   that mixed-zoom tiles show the same place twice, so label dedup becomes real
   work.
4. **`screenFor()` has to be allowed to fail.** Points behind the camera or
   above the horizon have no screen position. Four call sites, but the vehicle
   trail also needs clipping against the horizon or it draws garbage.

**Extruded buildings are the separate, additive project**, and they are what the
depth buffer is for. They also need height in the tessellator and
`render_height` carried through `map_build`/`map_rules` into the archive —
i.e. rebuilding the 512 MB archive. Pitch is worth having on its own; buildings
are only worth it once pitch exists.

## How it draws

The geometry is on the GPU; the text is not.

A QPainter-only renderer was built first and measured at **241 ms per frame**
(4 fps) on four z14 tiles — 14 440 features, 96 822 vertices — of which
buildings alone were 55 ms. That is a map you cannot pan.

The replacement is **QRhi driven directly against an offscreen texture**, which
is the one GPU path that survives `QT_QPA_PLATFORM=offscreen`:
`QOpenGLContext::create()` returns false there and `QRhiWidget` reports no RHI,
but Metal, Vulkan and D3D render into a *texture* without any window at all.
The frame is read back into a `QImage` and blitted by the widget's `paintEvent`.

Measured on the same scene: tessellation **1.9 ms once**, upload **6 ms once**,
and then a fraction of a millisecond per frame with pan, zoom and rotate.

The frame is **linear in pixels**, not flat — about **0.5 ms per megapixel** of
the target, so 660×640 is ~0.4 ms and the same widget at a device pixel ratio of
2 is ~1.0 ms. The cost is the readback rather than the rasterisation: forcing
`sampleCount` to 1 moves a 5120×2880 frame by under a millisecond, which is why
the sample count is taken as high as the hardware offers and the SIZE is the
thing to think about. Measure with `map_bench --width/--height/--dpr`.

There is **no CPU fallback**: without a backend the widget draws its background
and says so, which `status().gpuReady` reports.

Four things in that path are worth knowing before changing it:

- **The draw loop is layer-major across tiles**, not tile-major. Tile-major lets
  one tile's landcover paint over its neighbour's motorway, and the symptom is
  roads vanishing along tile seams — which reads as a tile-loading fault.
- **The uniform buffer is allocated once** and never grown. The pipeline holds a
  pointer to the shader resource bindings, which hold a pointer to the buffer;
  replacing the buffer destroys an object the pipeline still refers to, and the
  symptom is a frame that draws *nothing at all* while every draw call reports
  success. Hence `kMaxTilesPerFrame`, and truncation rather than growth.
- **A tile's geometry is compared by `TileGeometry::serial`, not by address.**
  Geometry freed by a cache eviction is routinely handed straight back by the
  allocator, so a replacement tile can land on the dead one's address — and an
  address comparison then says "unchanged" and keeps drawing stale triangles.
- **Tessellation happens on zenoh reply threads**, in `TileSource`, not on
  the GUI thread when a tile is first drawn. A reply carries up to 64 tiles at
  ~1.9 ms each, so it is spread across a small pool (`map/tile_workers.h`) and
  the reply thread blocks until they are all done — blocking rather than posting
  because the tile bytes are a span into the capnp reply, which dies with the
  callback. That works only because a config
  change rebuilds the widget, so the style is fixed for a `TileSource`'s life.
  If that ever stops being true, the geometry cache needs a style revision.

**Labels stay on the CPU**, and not for want of speed: they must *not* rotate
with the map (rotating text is unreadable at every bearing but north), and their
collision is viewport-global rather than per tile, so they cannot be baked into
the per-tile geometry the GPU caches. `map/labels.h` carries the argument.

Shaders are baked by `qsb` at build time into a header of bytes
(`cmake/EmbedBinary.cmake`) rather than a `.qrc`. A resource that fails to
register inside a static library linked into two executables presents as a
shader that will not load *at runtime*; a header cannot be dropped by a linker.

## Labels

Two kinds, placed by one pass so they compete for the same screen space.

**Point labels** come from the basemap's `place` layer and the tracks archive's
`track_label`. Ranked by the `rank` and `population` attributes `map_build`
writes rather than by the class name alone: the class cannot tell a town of
400 000 from one of 400, and where the city/town line falls is local convention
that moves by an order of magnitude between countries. A place big enough is
promoted a tier, using the **same thresholds** `map_rules` already uses to
promote its minZoom — promoting the zoom and not the rank is what drew a large
town from z6 and then labelled it under a city of 3 000. Promotion is upward
only, because a city tagged with a small population is far more often a stale
tag than a tiny city. An archive with no `rank` falls back to the class name, so
the bench archive still labels.

**Road names** come from `transportation_name`, and are the one line-placed
layer. Three things about them:

- **Laid along the road, at the middle of the longest visible run.** The
  longest run rather than the first, because MVT clips a road into as many
  parts as it has crossings of the tile edge and the first is as often as not a
  stub in a corner — and the pieces one way was clipped into are merged back
  together before "longest" is decided, so a road that leaves the tile and
  returns is one run rather than two fragments.

  Following the road means the run's geometry has to survive to the paint pass,
  which is why `LabelCandidate` carries a range into a flat arena of tile-local
  points rather than only an anchor. See **Curved road labels** below.
- **One label per name per viewport.** A road crosses every tile it passes
  through and each tile carries the whole name, so without this a single street
  is labelled a dozen times across one screen.
- **A name wider than the road it names is dropped.** This is what stops the
  two-metre stubs MVT leaves in tile corners from each claiming a label.

They rank between a neighbourhood and a locality: a street name must not push a
town off the map, but at the zooms it appears at, the street the driver is on is
worth more than a junction three miles away. Among themselves they order by
`roadPriority()` — the tessellator's own ladder, reused rather than restated so
the layer a road is DRAWN in and the weight its name carries cannot drift apart.

`detail.road_label` is the zoom floor, z14 by default, and `show_road_labels`
turns them off outright.

### Curved road labels

A road name is laid out **character by character along the road**, each glyph
rotated to the road's bearing where it sits. Three rules govern it:

- **Always upright.** The direction is decided once per label, from where the
  first and last characters land: if the name would read right-to-left across
  the screen, the same run is walked from the other end and every glyph turned
  180°. Never per glyph — flipping glyphs individually scrambles the word. This
  is what lets road labels rotate at all without breaking the rule the rest of
  the pass keeps: place names still never rotate, at any bearing.
- **Rejected where the road kinks.** The sum of the turn between consecutive
  characters, over any window of about 0.6 em of road, may not exceed 45°
  (MapLibre's `text-max-angle` default, and its reasoning). A sliding window
  rather than a per-glyph limit is the whole trick: a per-glyph threshold
  passes a spiral made of shallow steps and rejects one kink in an otherwise
  straight road.
- **Collision by several boxes, not one.** The axis-aligned hull of a diagonal
  word is mostly empty space and evicts neighbours it never touched, so a
  curved label claims one box per four characters. Straight labels still claim
  a single rotated box.

**The cost, and the one thing that controls it.** A straight name is one blit
of the cached whole-string image, exactly as before. A curved one is a rotated
blit *per character, twice* — halo pass then fill pass — and a rotated
`drawImage` measured about 4 µs. So which side of "straight" a label falls on
is worth roughly fifteen-fold, and `kStraightEnoughPx` is where that is decided.

It is asked in **pixels of deviation from the chord**, not in degrees, because
the question is "would drawing this as one straight image look different?" An
angle threshold gets it wrong in both directions: half a degree is invisible on
a short name and obvious on a long one, and real roads wander by fractions of a
degree constantly without looking bent. Asked in degrees at half a degree, two
thirds of the road labels in Irvine — a grid city — came out "curved".

Measured on the SoCal archive at 660x640, z14, M-series:

| | before | after |
|---|---|---|
| labels | 0.58 ms | 2.31 ms |
| whole frame | 1.34 ms | 3.1 ms |

24 labels placed, of which 13 genuinely curve. Bearing makes almost no
difference — a rotated straight road is still one blit — which is why
`map_bench` grew a `--bearing` flag but the number it reports barely moves.

**The halo must be a separate pass over the whole label.** Each glyph is cached
as two images and every halo is laid down before any fill. One combined sprite
per glyph would let the next character's opaque halo paint over the previous
character's text wherever they kern tightly, and the word comes out gnawed.

**Two things that are NOT done, deliberately.** A name is still placed once per
viewport rather than repeated every 250 px along a freeway the way MapLibre
does — that is a separate change with its own dedup consequences. And rivers
are not labelled at all, though `map_build` already emits `water_name` as a
LineString for exactly this treatment; once roads curve, rivers are a row in
`kLabelLayers` rather than a project.

## Running it

```bash
# check the archive without touching the bus
./build/nodes/map_server/map_server --config configs/map_server.yaml --check

# serve
./build/nodes/map_server/map_server --config configs/map_server.yaml
```

Or drive both through the agent interface, which supervises `map_server` as a
node — launched, read with `app_logs`, quit, but with no control socket, so
nothing that inspects or clicks applies to it:

```python
app_launch(app="map_server", config="configs/map_server.yaml")
app_launch(app="dashboard", config="configs/dashboard/map_demo.yaml")
```

Launch the node **first**. `app_launch` returns only once the server is
serving — it waits for the log line that means every service is registered, not
merely that the process survived — so the pair is not a race against an archive
still being opened. Without a server behind it the widget draws its background
and reports `No reply from map_server`, which is a screenshot that says nothing
about the map.

From another terminal:

```bash
./build/nodes/inspect/inspect services
./build/nodes/inspect/inspect call map/catalog --data '{}' --json

# Tiles are a BATCH -- `tiles` is a list, not z/x/y at the top level. The reply
# also carries the archive's minzoom/maxzoom, which is what the widget clamps
# its requests to.
./build/nodes/inspect/inspect call map/tile --json \
    --data '{"tileset":"socal","tiles":[{"z":14,"x":2828,"y":6562}]}'
```

That is the Irvine tile: 81958 bytes, gzip, 14 layers — the two the archive
carries that Irvine has none of are `mountain_peak` and `aerodrome_label`.

### Exercising the routing half

The one program that calls these for a living is `nodes/bd992_mock`: it asks
`map/route` for a road route and `map/nearest` for each segment's speed limit,
then drives the result and publishes it as GNSS. `bd992_mock --route ... --check`
exercises the whole routing half in one command and prints what came back — see
[bd992.md](bd992.md). What follows is the same thing by hand.

`map/graph` first, because it names the graph and the profiles the other two
calls have to match:

```bash
./build/nodes/inspect/inspect call map/graph --data '{"graph":"socal"}' --json
```

That reports `profiles: ["fastest"]` and the graph's bounds — for the SoCal
build, 5 001 449 segments and 9 501 530 edges.

`map/nearest` is the snap: which segments a coordinate could be on. It is also
where a route's time goes, so it is the one to reach for when routing is slow:

```bash
./build/nodes/inspect/inspect call map/nearest --json \
    --data '{"graph":"socal","latitudeDeg":33.6866,"longitudeDeg":-117.8558,
             "radiusM":50.0,"maxCandidates":3}'
```

`map/route` is the search. Irvine to UC Irvine, about 7.3 km:

```bash
./build/nodes/inspect/inspect call map/route --json \
    --data '{"graph":"socal","profile":"fastest",
             "fromLatitudeDeg":33.6866,"fromLongitudeDeg":-117.8558,
             "toLatitudeDeg":33.6405,"toLongitudeDeg":-117.8443,
             "simplifyToleranceM":5.0}'
```

Knobs worth exercising, and what each one changes:

| change | what it shows |
|---|---|
| `"hasFromHeading":true,"fromHeadingDeg":0` vs `180` | the departure heading ranks the start candidates — 7329 m northbound against 7498 m southbound, a genuinely different route |
| `"simplifyToleranceM":0` vs `5` | geometry precision: 414 points against 277, same distance |
| a destination in Santa Barbara (`34.4208,-119.6982`) | a 217 km, 2 h 05 search rather than a local one |
| a coordinate in Denver | `noMatch`, *"no road near the start"* — the coverage edge |
| `"graph":"nope"` | `noSuchGraph`, distinct from a graph that is configured and will not open |

A route is **geometry and segment ids, and nothing above that** — there are no
turn instructions, and no service resolves a segment id to the name the graph
holds for it. Names, refs and road class reach a client only through
`map/nearest`.

`geometry` is a flat list of **interleaved lon,lat** in units of 1e-7 degrees,
so `[-1178556935, 336866382]` is -117.8556935, 33.6866382. `segmentStarts`
indexes into it, one entry per segment plus a final end, so segment *i* owns
points `[segmentStarts[i], segmentStarts[i+1])`.


## Where an archive comes from

`tools/map_build` — see [Building the map](map_build.md). It reads an OSM PBF and
writes the tiles, the routable road graph and the routing overlay from one
classification pass, so the drawn road and the routable road can never disagree.
`tilemaker` is no longer in the loop.

## Adding a tileset

One entry in `configs/map_server.yaml`:

```yaml
tilesets:
  - name: socal
    path: /Users/ryan/Documents/map_data/socal.mbtiles
```

The **name** is what clients ask for; the path is nobody's business but the
node's. Two tilesets with one name, or a name containing `/`, are refused at
startup — both would otherwise produce a server that starts cleanly and answers
wrongly.

## XYZ in, TMS on disk

The mbtiles spec stores **TMS** rows: `tile_row` counts northward from the
bottom. Everything else here — the projection, the capnp request, the widget —
uses **XYZ** (slippy), where `y` counts southward from the top.

The conversion is `row = 2^z - 1 - y`, and it happens in exactly one place:
`mbtiles::Archive::tile()`. Nowhere else in the tree may flip a tile coordinate.

This gets its own section because of how it fails. A wrong flip does not throw,
log or return nothing — it returns a real tile from the wrong hemisphere, and
the map renders beautifully, mirrored about the equator.
`mbtiles_test_archive` pins it by writing archives in TMS and asserting in XYZ;
inverting the flip in `archive.cpp` makes 20 checks fail.

## The vector tile decoder

`libs/mvt` is a minimal protobuf reader plus the MVT geometry grammar. No
protobuf library: the `.proto` is eleven fields across four messages, unchanged
since 2016, and pulling in libprotoc plus a codegen step for that would be a
larger dependency than the format it reads.

Five things about the format are easy to get subtly wrong, and all five render
rather than fail:

| Trap | What it looks like |
|---|---|
| `extent` is **per layer**, default 4096 | one layer drawn at the wrong scale |
| Coordinates may fall **outside** `0..extent` (tile buffer) | a seam down every tile boundary |
| Polygon **ring winding** decides exterior from hole | every lake with an island fills solid |
| The geometry **cursor persists across commands** | every road collapses onto the tile corner |
| `ClosePath` does **not** repeat the first point | polygons drawn with one edge missing |

`mvt_test_decode` covers each. `mvt_test_real_tiles` decodes real
output from the archive named in `configs/map_server.yaml`, and **skips loudly**
when that 383 MB file is absent so a fresh checkout still passes.

## Projection

Web Mercator, in `dashboard/widgets/map/projection.h`. World coordinates are
normalised to `[0,1]` so the zoom appears exactly once per conversion.

The anchor for the tests is Irvine at `z14/2828/6562`, worked out by hand from
the Mercator formula. It is confirmed independently: `libs/mvt` pulls that same
tile out of the real archive and finds a city's worth of roads in it. A wrong
projection would name a tile that is empty ocean.

Latitude is clamped to ±85.0511287798 — past that Mercator's `tan()` runs to
infinity, every subsequent arithmetic yields NaN, and the map paints nothing at
all, silently.

## The widget

Registered through `DASHBOARD_WIDGET_TABLE`, so it appears in the editor's
palette and is inspectable and settable through `widget_*_config` with no extra
work.

**The style is a reflected struct**, not a style document. `MapStyle_t` is a
colour, a width and a zoom threshold per kind of thing, editable in the
properties panel and patchable over `widget.set_config`. A GL style is a small
programming language — filters, expressions, zoom interpolation — and
implementing it faithfully is a larger job than the renderer it configures.

Everything is optional; omitting a key keeps the default in `map/style.h`.

Two things do not fit the one-knob-per-layer shape and are worth knowing.
**Boundaries take their weight from `admin_level`**, as a fraction of
`widths.boundary` — a country border is heavier than a city limit, but there is
still only one knob for the set, and setting it to zero still hides all of them.
An archive that does not write the attribute keeps a single weight rather than
dropping to the thinnest. **Aeroways are three layers from one source layer**:
the apron surface as a fill, and runways and taxiways as lines at different
widths, because a runway is tens of metres of concrete and a taxiway is a lane —
drawn at one width, either the runway stops reading as the landmark that
identifies an airport or every taxiway becomes a second runway.

| group | what it holds |
|---|---|
| `style.*` | 19 colours, `label_font`/`label_size`/`label_halo_width`/`label_spacing`, `road_width_scale`, the five `show_*` toggles |
| `style.widths.*` | per-layer half-width in px at z14 — motorway, casing, primary, major, minor, rail, runway, taxiway, waterway, boundary, racetrack centre. **0 hides the layer** and costs nothing: no vertices, no upload, no draw call |
| `style.detail.*` | per-layer lowest zoom, 18 layers plus `road_label`. The clutter dial |
| `marker_*`, `track_*` | the vehicle marker, its outline and its trail |

Two things about that surface are worth knowing:

- **`road_width_scale` rides the shader uniform, not the vertices.** Widening
  every road is a uniform write, not a re-tessellation of the city. It must
  therefore be applied in exactly one place — doing it in the tessellator *and*
  the uniform squares it, so a scale of 2 draws roads four times too wide.
  `test_road_width_scale_is_applied_exactly_once` is there because that bug
  existed.
- **`style.detail` can only ever be stricter than the archive.** Lowering
  `detail.building` below z13, or `detail.water` below z6, draws nothing extra —
  nothing wrote those features at those zooms. The build-time dial is the
  per-class `minZoom` in `libs/map_rules`; raising a threshold here is the useful
  direction and is free. See the split above.

**`paintEvent` composites three passes**: the GPU frame, then labels, then the
vehicle marker and its trail. There is deliberately no `CachedPaintWidget` and
no cached underlay — that split exists to keep an expensive redraw off the hot
path, and both expensive parts here are already cached somewhere better.
Tessellation happens once per tile on a zenoh thread inside `TileSource`, and a
frame whose camera, tiles and style are unchanged comes straight back out of
`GpuRenderer`'s own memo without being drawn or read back at all. A pixmap here
would be a third copy of the same picture.

**The GPU pass renders at the screen's device pixel ratio**; the other two are
QPainter and are drawn at it anyway. `Projection` carries the ratio without
applying it — every coordinate it returns is logical, which is what the label,
marker and tile-selection passes need — and `GpuRenderer::render()` is the only
thing that multiplies by it, for the target size, the tile placement and the
road width uniform. The frame it returns carries the ratio on the `QImage`, so
the widget's blit covers the same rectangle either way.

That is not free. The frame is close to linear in device pixels — about **0.5 ms
per megapixel** on an M-series Metal backend — so a 660×640 widget goes from
~0.4 ms to ~1.0 ms at 2x, and a 2560×1440 one from ~1.8 ms to ~7.4 ms. The cost
is the readback, not the rasterisation: forcing `sampleCount` to 1 moves a
5120×2880 frame by under a millisecond. Measure it with
`map_bench --width … --height … --dpr …`.

A viewport wide enough that the device-pixel texture would exceed the 8192 px
limit has its ratio lowered until the frame fits, rather than failing and
leaving a blank widget with no diagnostic. `status().gpu.devicePixelRatio`
reports what the last frame was actually rendered at, which is the only evidence
that happened.

For a BD992:

```yaml
- type: map
  id: nav
  config:
    tileset: socal
    position_zenoh_key: nodes/bd992/position
    position_schema_type: GsofLatLongHeight
    latitude_expression: latitudeDeg
    longitude_expression: longitudeDeg
    follow_vehicle: true
```

`configs/dashboard/map_demo.yaml` is a working layout with two of them.

## Panning and zooming

Off by default. `interactive: true` turns on dragging to pan and the wheel to
zoom — a dashboard is a surface people brace a hand against on a bad road, so a
layout has to ask for a map that moves.

Both gestures go through `Projection::worldForScreen()`, which is what makes
them anchored rather than approximate: a drag keeps the point you grabbed under
the pointer, and the wheel keeps the point under the pointer where it is while
the scale changes. Anchoring in world units also means it stays correct under
rotation — a screen-space delta sends a bearing-rotated map off at an angle to
the drag.

The camera has three sources, in order: where the user dragged to, then the
vehicle if `follow_vehicle` is on, then the configured centre.

- **A drag suspends Follow Vehicle**, and having a dragged-to centre *is* what
  suspended means — there is no second flag to keep in step with it. The
  alternative is a map that snaps back on the next position fix and cannot be
  looked away from at all.
- **The wheel does not.** While Follow Vehicle is on, zooming anchors on the
  centre instead of the pointer, so the vehicle does not move on screen and
  there is nothing to suspend. This is deliberately not conditioned on a
  position having arrived: following is a declared intent, not a state that
  waits for a fix, and a zoom taken while the GPS is still coming up must not
  cancel it for good.
- Zoom is clamped to `[min_zoom, max_zoom]`, which bound the **camera** and
  nothing else — see *Who decides the zoom range* below.
- The panned centre is clamped to the Mercator limit and wrapped at the date
  line, so a drag cannot leave the projection.

`RecentreButton` appears in the bottom right once the camera has been moved,
and drops the pan when clicked. It does **not** touch the zoom: the user chose
that separately, and asking to be recentred is not asking to be zoomed back
out. It is a real `QAbstractButton` rather than a rectangle painted into
`paintEvent`, which buys hover and press states, makes the editor's recursive
mouse-transparency (`Canvas::setEditorMode`) cover it in edit mode, and puts it
in `ui_snapshot` as an addressable widget instead of a coordinate the agent
interface has to be told about. It paints itself in the style's *label* colours
— the pair already chosen to stay readable over an arbitrary map.

`map_test_widget_hidpi` aside, an offscreen widget has no screen and so no
mouse; the interaction tests post `QMouseEvent`/`QWheelEvent` straight at the
widget, which reaches the same handlers Qt would.

## Not blanking while tiles are in flight

Drawing only what has arrived is what makes a map flash its background on every
zoom, and it reads as a fault rather than as loading. The data to avoid it is
usually already in hand: `TileSource`'s cache is keyed by `TileId` **across
zooms** and is never cleared on a camera change, so zooming in leaves the
shallower tiles you came from and zooming out leaves the deeper ones. And a tile
drawn at a zoom other than its own needs no special handling at all —
`tileOrigin()` and `tileScreenSize()` place it from its own id.

So each paint, `substituteTiles()` asks the cache what can go **under** the gaps:

- **Ancestors first**, nearest wins. One covers a tile and its three siblings,
  so four missing tiles usually cost one extra draw call between them. Measured
  against the real archive: six missing z12 tiles were covered by **two** z11
  ancestors.
- **Then one level of descendants.** They partition their parent, so unlike an
  ancestor they overlay nothing. This is the zoom-*out* case, and one level is
  the cap because two is sixteen tiles for one — a zoom gesture moves a level at
  a time anyway.

Stand-ins go **first** in the batch list, so within each layer pass a real tile
covers its own ground. Where an ancestor spans a tile that *did* arrive they
overdraw, which is harmless: an ancestor is the same geography more simply
drawn, so the two coincide.

Three things worth knowing:

- **Stand-ins label too, but only after every real tile**, and that order is the
  whole trick. Leaving them out of the label pass — the first thing tried — made
  the text blink out for the frames a zoom was in flight while the geometry
  underneath stayed, which is a worse artefact than the blank map this was meant
  to fix. Duplicates take care of themselves: a place named by both an ancestor
  and the real tile lands on the same pixels, and `paintLabels()` rejects a
  candidate colliding with one already placed, so real tiles going in first
  decides which wins. Measured through a z11→z12 transition: 18 labels during,
  18 after, in the same positions.
- **The diagnostic caption knows about them.** `paintDiagnostic()` explains a
  map with *nothing* on it, so it tests `tilesDrawn` **and** `tilesStandIn` —
  without the second it captions a perfectly good stand-in frame with "No
  coverage here in tileset 'socal'", which is a line of text flashing over
  visible roads.
- **It is budgeted.** The renderer draws at most `kMaxTilesPerFrame` and
  truncates the *tail*, where the real tiles are, so stand-ins are capped at
  what is left over. `status().tilesStandIn` reports how many were used; a
  number that stays non-zero means tiles are not arriving.
- **Panning at a constant zoom is not covered**, and cannot be by this
  mechanism: the leading edge's ancestor is only cached if you happened to view
  that ground at a shallower zoom. The one-tile prefetch ring covers a pan of
  about a tile; a **coarse overview cover four levels down** is requested
  alongside it, at the END of the request list so it can only spend slots the
  viewport did not want, and that is what covers ground never visited at any
  zoom. Four levels because one overview tile spans 16x16 of the drawn ones, and
  because `kMaxSubstituteLevelsUp` is five — an overview deeper than the
  substitute walk looks would be fetched and never drawn.

Zooming *in* past the archive never produces a gap at all, for a different
reason — `tileZoom()` clamps to what the archive holds, so the tiles are already
the ones on screen, just magnified.

## Who decides the zoom range

Two different questions, and the widget stopped conflating them:

| | who answers | what it means |
|---|---|---|
| which tile level to fetch | **the server**, on every reply | what the archive actually holds |
| how far the camera may zoom | `min_zoom`/`max_zoom` in the layout | how close a layout lets the user get |

`MapTileResponse` carries `minzoom`/`maxzoom` on every reply, `TileSource` keeps
the first pair it sees, and `refreshTiles()` clamps `tileZoom()` to it. So a
layout no longer transcribes a number that has to match an `.mbtiles` file on
another machine — point a widget at a deeper archive and it uses the depth.

**Why the reply carries it rather than a catalogue call, and rather than
nothing at all.** A client cannot infer the depth from the per-tile answers:
most of the pyramid is empty, so a hole over the desert at z14 and a level the
archive never had both come back absent. Left to work it out, a client wanting
to zoom past the archive would walk down a level at a time — one round trip
each — and would still walk all the way to zero over genuinely uncovered
ground. Two bytes on a reply that already carries a hundred kilobytes of tile
removes all of that, and the first reply of a session is enough.

`checkTileRange()` in `tilesets.h` is what separates the three answers, and
`map_server_test_tile_range` pins them:

- **badRequest** — not a tile coordinate at all (z past 22, x/y outside 2^z).
  A client bug. Screened *before* anything shifts by `z`, because `1U << 40` is
  undefined rather than large.
- **outOfRange** — a real coordinate at a level this archive lacks. The answer
  exists shallower; ask there.
- **notFound** — the archive covers that level and has nothing at that
  coordinate. Final; draw nothing.

Getting the middle two the wrong way round fails quietly in both directions: a
blank map the moment anyone zooms past the archive, or a client retrying
uncovered ground a level up, and again, all the way to zero, once per tile.

Two consequences worth knowing:

- **Overzoom is cheap; underzoom is not.** A camera deeper than the archive
  looks at a *fraction* of one tile — free, and magnified vector tiles stay
  sharp, because the geometry is triangles in tile-local coordinates and roads
  are expanded in the shader to a screen-pixel width. Measured against the
  SoCal archive (z14): z16 is indistinguishable in sharpness, z17 still reads
  as streets, and by z18 there is too little left in frame to be a map — hence
  a default `max_zoom` of 17. A camera *shallower* than `minzoom` is the
  opposite: 256× the tiles at four levels out, truncated by `kMaxVisibleTiles`
  into a partly drawn map. Raise `min_zoom` to the archive's floor if that
  matters.
- **Learning the range has to trigger a repaint.** When the camera is parked
  past the archive, every tile comes back `outOfRange`, nothing lands in the
  mailbox, `drain()` returns zero — and without
  `TileSource::takeArchiveRangeLearned()` the widget would sit blank holding a
  range it never redrew with. That bug was real; it is why the flag exists.

## Diagnosing a blank map

`MapWidget::status()` separates the causes that look identical in a screenshot,
and the widget draws the answer on itself when `show_status` is on:

| Message | Means |
|---|---|
| `No GPU backend — map geometry cannot be drawn` | `QRhi::create()` failed for every backend; labels and marker still draw |
| `No tiles requested` | the widget has no size yet |
| `No reply from map_server on 'map/tile'` | the node is not running, or the key is wrong |
| `Waiting for tiles…` | requests are out, nothing back yet |
| `No coverage here in tileset 'socal'` | tiles arrived and the archive is empty here |

`status()` also reports `camera` — where the map is *actually* looking, which
is not always the configured centre once Follow Vehicle or a drag has moved it
— and `cameraMoved`, which says a pan is in effect. A screenshot of the wrong
place and a screenshot of an archive with no coverage there are the same
picture.

## The tile cache, and what leaves it

`map/tile_cache.h`, split out of `TileSource` because that class cannot be built
without a zenoh session and the eviction policy is the part worth testing.

Bounded by **both** a tile count (256) and a byte budget (128 MB), because
neither works alone: an empty ocean tile is a few dozen bytes and a downtown z14
tile is a few hundred kilobytes, so a count alone permits anywhere between
nothing and hundreds of megabytes depending on where the drive went — while a
byte budget alone would let a city of tiny tiles grow the map without limit.

**Least recently used, and asking counts as use.** `ready()` and `drawable()`
promote what they are asked about, which is every tile the paint pass looks at.
Insertion order — what this was — evicts the tile under the vehicle in favour of
one the drive left behind an hour ago, and an evicted tile that is still on
screen is re-fetched immediately: the cost is a round trip and a re-tessellation
every frame rather than once. `contains()` is the exception and deliberately
does NOT promote: `request()` asks it of the whole viewport plus its prefetch
ring on every paint, and treating that as use would protect exactly the
speculative ring tiles that should go first.

**The floor wins over the byte budget.** Eviction stops at 80 tiles however
heavy they are, because a viewport bigger than the budget would otherwise evict
tiles it is still drawing and re-request them on the next paint — a refetch loop
that reads as a slow server rather than as a small cache. With worst-case tiles
that knowingly exceeds the budget. Memory spent holding the screen is worth more
than memory saved thrashing it.

## What a frame costs away from Apple silicon

The numbers elsewhere in this file were measured on an M-series Metal backend.
On a Linux box with an Intel UHD 630 through Mesa, measured with `map_bench`
against the real SoCal archive at the dashboard's own 660x640:

| | this box | M-series (above) |
|---|---|---|
| gpu render | 4.83 ms | ~0.4 ms |
| labels | 2.79 ms | — |
| whole frame | 8.43 ms | — |

**The GPU frame is about twelve times slower, and it is the readback**, exactly
as the rest of this file warns. Fitted across three viewport sizes -- 0.11,
0.42 and 1.69 megapixels -- it comes to:

    gpu_frame = 2.44 ms fixed + 3.51 ms per megapixel

Two things separate that from Apple silicon. The per-megapixel term is **seven
times** the 0.5 ms/Mpx measured on Metal, because `readBackTexture` there is a
copy inside unified memory while here it crosses the bus from the iGPU. And
there is a **2.44 ms fixed cost that Metal does not have at all**, which is the
synchronous `endOffscreenFrame()` pipeline stall: cheap on a tiler with shared
memory, expensive on Mesa.

The consequence is worth stating plainly, because it inverts the advice above:
on this hardware the fixed cost dominates at dashboard sizes, so **making the
viewport smaller buys much less than the M-series numbers suggest** -- halving
the linear dimensions took 4.83 ms to 2.08 ms, not to a quarter.

**Labels are fine.** 2.79 ms for ~450 candidates and ~25 placed, with the
render cache hitting every time after the first frame. A single label costs
1.27 ms to stroke and fill here against the 0.88 ms recorded on macOS, so the
raster engine is ~1.4x slower and nothing more.

Those figures predate curved road labels, which cost about 1.7 ms more on the
M-series box — see **Curved road labels** above. Nothing has re-measured them
on this hardware; if the ~1.4x holds, expect the label stage nearer 5 ms here,
which is why `kStraightEnoughPx` is the first knob to reach for on a slow
raster engine.

**A benchmarking note, learned the hard way.** `map_bench` is CPU-bound in the
label pass and GPU-bound in the readback, so two of them running at once report
each other's contention rather than the frame's cost. Concurrent runs on this
box produced a label figure of 476 ms -- a hundred and seventy times the real
one -- and it looked entirely plausible. Run it alone, and check `uptime` first.

## Things that are load-bearing

- **A line thinner than a pixel is widened to one and faded, not drawn thin.**
  `widthScaleForZoom()` tapers to 0.15, so a 1.5 px half-width is 0.22 px at low
  zoom — and a sub-pixel line does not land on pixel centres reliably. Measured
  before the floor: a 0.1 px half-width swept over one pixel of camera offset
  gave ink between 0.0 and 32.1, i.e. gone entirely at some positions and back
  at others, which reads as crawling when the map moves. No sample count fixes
  that. `map.vert` widens to half a pixel either side and takes the difference
  out of ALPHA, so total ink is unchanged — widening without fading turns a
  continental view into a solid mat of roads. Fills carry a zero half-width and
  are explicitly excluded.

- **A missing tile is `NOT_FOUND`, not an error, and is cached as an empty
  tile.** Most of the pyramid is empty; without caching the miss, the widget
  re-requests it every frame forever.
- **The tile-arrival callback runs on a zenoh thread.** It sets one atomic and
  posts a single queued invoke; every later arrival in the burst sees the flag
  set and posts nothing. Without that gate it is one `QMetaCallEvent` per tile,
  and a pan across a city is a few hundred — which is how the CarPlay widget
  fell behind before it was rewritten around a mailbox.
- **The widget works out its tiles in the paint pass, not on resize.** Qt does
  not deliver a resize event to a widget that has never been shown, so a widget
  sized by a layout before it appears would otherwise keep the tiles it computed
  at Qt's default 640×480 — and draw them, at the wrong scale, forever.
- **Never use `uint8_t` in a reflected config.** yaml-cpp treats `unsigned char`
  as a *character* type: a zoom of 14 is written as the unprintable byte `0x0E`
  and read back as a bad conversion, which throws out of the YAML decoder and
  takes the whole layout with it. `min_zoom`/`max_zoom` are `uint16_t` for
  exactly this reason.
- **The vendored `sqlite3` stays out of the GUI apps.** `libs/mbtiles` is
  server-side only; the widget never links it, because it asks the node.
- **`Qt6::GuiPrivate` is what exposes `<rhi/qrhi.h>`.** QRhi is semi-public in
  Qt 6 — a stable-ish API shipped behind the private headers — and the
  `InitParams` structs live in `<rhi/qrhi_platform.h>`, each behind the feature
  test for its own backend. `QT_CONFIG(vulkan)` alone is **not** enough to
  reach `QRhiVulkanInitParams`: Qt reports the feature on macOS, where there is
  no `vulkan.h` and the type is never declared. Mirror the header's own guard.
- **earcut.hpp is included behind a `-Wshadow` pragma**, not marked SYSTEM. It
  shadows its own members in a dozen places, which trips this tree's `-Werror`;
  silencing it for the header alone keeps `-Wshadow` live for our code and
  keeps the include out of Homebrew's `-isystem` search order.
