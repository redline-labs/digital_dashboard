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
the replacement gives up: **no 3D** — see below — no GL style documents, and
labels that do not follow road curves.

What it gains, beyond working offscreen: **no glyph PBFs**. `QPainter` has
fonts, so there is no font pipeline, no sprite sheet, and no "the map has no
labels and nothing said why" failure mode.

## What it does not do: 3D

The renderer is **2D only**, by construction rather than by omission:

- `MapVertex` carries `x, y` and a 2D normal. There is no z, no depth buffer,
  and no depth test — correctness comes from painter's-algorithm draw order
  (layer-major across tiles), which only works because everything is flat.
- `Camera` has `center`, `zoom` and `bearing`. There is **no pitch**, so the
  view is always straight down; the MVP is an orthographic projection with a
  rotation about z.
- Buildings are drawn as **footprints**, not extrusions. The data is there and
  unused: the `building` layer in the bench archive carries `render_height` and
  `render_min_height`, and the tessellator reads neither.

Adding 3D would mean a pitch angle on `Camera`, a perspective MVP, a depth
buffer on the render target, extruded wall geometry, and per-face lighting —
and it would break the flat draw-order model that the current correctness
argument rests on. It is a rewrite of the draw path, not a feature flag. There
is no plan for it: a dash map is read at a glance, and a tilted view trades
legibility for looks.

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
and then **0.22 ms per frame** with pan, zoom and rotate — and flat in
resolution (0.23 ms at 2560×1440), because the cost is draw-call submission
rather than fill rate. Readback is noise (0.40 ms vs 0.44 ms without it) on
unified memory. There is **no CPU fallback**: without a backend the widget draws
its background and says so, which `status().gpuReady` reports.

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
- **Tessellation happens on the zenoh reply thread**, in `TileSource`, not on
  the GUI thread when a tile is first drawn. That works only because a config
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

## Running it

```bash
# check the archive without touching the bus
./build/nodes/map_server/map_server --config configs/map_server.yaml --check

# serve
./build/nodes/map_server/map_server --config configs/map_server.yaml

# look at it
app_launch(app="dashboard", config="configs/dashboard/map_demo.yaml")
```

From another terminal:

```bash
./build/nodes/inspect/inspect services
./build/nodes/inspect/inspect call map/catalog --data '{}' --json
./build/nodes/inspect/inspect call map/tile \
    --data '{"tileset":"socal","z":14,"x":2828,"y":6562}' --json
```

That last one is the Irvine tile: 81958 bytes, gzip, 14 layers — the two the
archive carries that Irvine has none of are `mountain_peak` and
`aerodrome_label`.

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

| group | what it holds |
|---|---|
| `style.*` | 16 colours, `label_font`/`label_size`/`label_halo_width`/`label_spacing`, `road_width_scale`, the three `show_*` toggles |
| `style.widths.*` | per-layer half-width in px at z14 — motorway, casing, primary, major, minor, rail, waterway, boundary. **0 hides the layer** and costs nothing: no vertices, no upload, no draw call |
| `style.detail.*` | per-layer lowest zoom, 12 layers. The clutter dial |
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

## Things that are load-bearing

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
