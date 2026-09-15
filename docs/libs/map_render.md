---
title: map_render
parent: Libraries
---

# map_render

## Overview

`libs/map_render` is everything that turns a vector tile into pixels: the Web
Mercator projection and tile arithmetic, the tessellator, the QRhi pass, label
extraction and layout, the decoded-tile cache and the worker pool that spreads
tile decode across cores. It knows nothing about where the bytes came from.
The dashboard's map widget (`libs/dashboard_widgets/widgets/map`) asks
[map_server](../nodes/map_server.html) over zenoh; `scope`'s map panel reads
an `.mbtiles` directly. Neither transport is visible from inside this library,
and neither is `libs/mbtiles`: the archive reader stays with whoever opens a
file. Tiles arrive already decoded by [mvt](mvt.html).

It also deliberately holds no `QWidget`. `MapPass` records the drawing and owns
no target; `OffscreenRenderer` hosts it against an offscreen texture rather
than a platform surface, which is the whole reason a map draws at all under
`QT_QPA_PLATFORM=offscreen`, where every `gui` test and every agent screenshot
runs. The `QRhiWidget` host that draws the same pass straight into a window is
`libs/map_surface`, kept separate so this target stays buildable and testable
headless. The renderer was hoisted out of the widget the way `libs/config_codec`
and `libs/qt_helpers` were hoisted out of the dashboard when `scope` was
created, and for the same reason: a second top-level app must not reach into
the first's include tree. Why the renderer is ours at all, and what a frame
costs, is in [Map renderer](../design/map-renderer.html).

## Public headers

| Header | What it holds |
|---|---|
| `projection.h` | Web Mercator, `Camera` (centre, zoom, bearing, pitch up to `kMaxPitch = 60`), `TileId`, visible-tile walks, stand-in substitution, `kMaxSubstituteLevelsUp` |
| `camera_modes.h` | `MapViewMode_t` (`top_down`, `perspective`), shared by both surfaces; orientation modes are per surface and not here |
| `style.h` | `MapStyle_t`, `MapWidths_t`, `MapDetail_t`: the reflected style struct and its `validate()` |
| `tessellator.h` | Vector tile to triangles in tile-local coordinates, once per tile; earcut polygons with holes, mitre-joined lines, `roadPriority()` |
| `map_pass.h` | The GPU pass: every resource and draw call, no target; `kMaxTilesPerFrame`, `Stats` |
| `offscreen_renderer.h` | The offscreen host: texture, frame memo, readback to `QImage` |
| `buffer_arena.h` | First-fit suballocator so a resident tile's geometry stays put when the visible set changes |
| `label_candidates.h` | `LabelCandidate`, `LabelSet` (candidates plus the flat arena of tile-local run points), `extractLabels()`; QtCore only |
| `labels.h` | Placement, collision and the glyph atlas: `LabelCache` |
| `text_quad.h` | One placed character on its way to the GPU; the seam between `labels.h` and the pass |
| `tile_cache.h` | The decoded-tile cache and its eviction policy; GUI thread only |
| `tile_workers.h` | A fixed pool running an indexed job and waiting |

## Using it

The CMake target is `map_render`. An embedder picks a host by choosing a
library: link `map_render` alone and drive `OffscreenRenderer::render()`
yourself, as `map_bench` and the tests do, or link `libs/map_surface` and add a
`MapSurface` child, which picks between `RhiWidgetHost` and `OffscreenHost` in
its constructor and never says which. On a real display the `QRhiWidget` host
records into the widget's own swapchain with no readback; offscreen, the
texture-and-blit host is the only one that exists.

Per tile, on a decode worker off the GUI thread: `mvt::inflateIfCompressed`,
`mvt::decode`, then `tessellate()` into a `TileGeometry` and `extractLabels()`
into a `LabelSet`, both kept by `TileId` across zooms in `TileCache`. Per frame
on the GUI thread: build a `Projection` from the camera and viewport, walk
`visibleTilesWithMargin()`, fill gaps with `substituteTiles()`, lay out text
with `LabelCache`, and hand batches and text quads to the host. `map_bench.cpp`
is the shortest complete example, and its include list is the API surface:

```cpp
#include "map_render/offscreen_renderer.h"
#include "map_render/labels.h"
#include "map_render/projection.h"
#include "map_render/tessellator.h"
```

**Embedding through `MapSurface`.** The surface is
`WA_TransparentForMouseEvents`, so gestures land on the embedder, and floating
chrome must stay a child of the embedder: Qt's hit test does not descend into a
transparent-for-mouse subtree, so a button parented inside the surface draws
and is never clickable. A `QRhiWidget` has no paint engine, so the embedder's
marker and diagnostics go through `setOverlayPainter()` onto a transparent
layer above the map; `refreshOverlay()` repaints that layer without redrawing
the map, so a moving vehicle over a still map costs no GPU work.

**How the dashboard widget uses it.** `MapWidget::paintEvent` is the frame
driver: it ticks the eases, works out the camera, asks `TileSource` for tiles,
lays out labels and submits, and puts no pixel on screen itself. There is no
`CachedPaintWidget` and no cached underlay, because both expensive parts are
already cached better: tessellation happens once per tile on a zenoh reply
thread inside `TileSource`, and a frame whose camera, tiles, style and text are
unchanged comes straight back out of the renderer's memo. The GPU pass renders
at the device pixel ratio; `Projection` carries the ratio without applying it,
so every coordinate it returns is logical, and `OffscreenRenderer::render()` is
the only thing that multiplies by it. A viewport whose device-pixel texture
would exceed 8192 px has its ratio lowered until the frame fits rather than
going blank, and `status().gpu.devicePixelRatio` is the only evidence of it.

**Corner controls** are `libs/map_controls`, a sibling target because they are
`QWidget`s. `MapButton` is the base: a translucent disc in the style's label
colours, a hand-painted shadow, hover and press through opacity. Subclasses
(`RecentreButton`, `CompassButton`, `ViewModeButton`, `ZoomButton`) paint only
their glyph, and `layOutStack()` stacks whichever are visible into a corner,
shedding from the far end of the caller's list when the host is too short.

## Behaviour worth knowing

**Projection.** World coordinates are normalised to `[0,1]` so the zoom
appears exactly once per conversion; screen coordinates are logical pixels.
Latitude is clamped to ±85.0511287798, past which Mercator's `tan()` runs to
infinity and the map paints nothing, silently. Everything here is XYZ (slippy,
`y` southward from the top); the one TMS flip in the tree is in
[mbtiles](mbtiles.html). The test anchor is Irvine at `z14/2828/6562`, worked
by hand and confirmed by `libs/mvt` finding a city's worth of roads in that
tile of the real archive.

**The draw path.** The loop is layer-major across tiles; tile-major lets one
tile's landcover paint over its neighbour's motorway, which reads as roads
vanishing at seams. The uniform buffer is allocated once and never grown: the
pipeline holds a pointer to the bindings, which hold a pointer to the buffer,
and replacing it makes a frame that draws nothing while every call reports
success, hence `kMaxTilesPerFrame` and truncation. Geometry is compared by
`TileGeometry::serial`, not by address, because a replacement tile routinely
lands on an evicted one's address. Shaders are baked by `qsb` into headers of
bytes (`cmake/EmbedBinary.cmake`) rather than a `.qrc`, because a resource that
fails to register inside a static library linked into two executables presents
as a shader that will not load at runtime. There is no CPU fallback: without a
backend the map is its background and `status().gpuReady` says so.

**Labels.** Placement and collision stay on the CPU because collision is
viewport-global; drawing is textured quads from an atlas holding what
`QPainter` rasterised, so QPainter is still the font stack and there are no
glyph PBFs. Text is part of the frame memo's key. Each glyph is cached as two
images and every halo is laid before any fill, or tight kerning gnaws the word.
`LabelCache` compares a `StyleKey` struct by value and deliberately not via
`QFont::key()`, which formats a `QString` and cost 4 ms a frame when asked once
per character. A road's run reaches the paint pass as a range into
`LabelSet::path`, one flat arena per tile, not a vector per candidate.

**Style.** `road_width_scale` rides the shader uniform and must be applied in
exactly one place; doing it in the tessellator too squares it, and
`test_road_width_scale_is_applied_exactly_once` exists because that bug did.
`style.detail` can only be stricter than the archive: lowering
`detail.building` below z13 draws nothing extra. A width of 0 hides a layer and
costs nothing.

**Tiles.** `TileCache` is bounded by count (256) and bytes (128 MB), is LRU
where `ready()`/`drawable()` count as use and `contains()` does not, and never
evicts below 80 tiles. Gaps are covered by `substituteTiles()` from ancestors
first, then one level of descendants; stand-ins are drawn and labelled after
real tiles. A missing tile is `notFound`, not an error, and is cached as empty
so it is not re-asked every frame. In the widget, `TileSource` tessellates on
the zenoh reply thread: a reply carries up to 64 tiles at about 1.9 ms each,
spread across the `tile_workers.h` pool, and the reply thread blocks until they
are done rather than posting, because the tile bytes are a span into the capnp
reply that dies with the callback. That works only because a config change
rebuilds the widget, so the style is fixed for a `TileSource`'s life; if that
ever stops being true, the geometry cache needs a style revision. The arrival
callback sets one atomic and posts a single queued invoke per burst, and tiles
are worked out in the paint pass rather than on resize, because Qt delivers no
resize to a never-shown widget.

**Hairlines.** A line thinner than a pixel is widened to one and faded, never
drawn thin: `widthScaleForZoom()` tapers to 0.15, and a 0.1 px half-width swept
over one pixel of camera offset gave ink between 0.0 and 32.1, which reads as
crawling. `map.vert` takes the difference out of alpha so total ink is
unchanged; fills are excluded.

{: .warning }
Never use `uint8_t` in a reflected config. yaml-cpp treats `unsigned char` as
a character: a zoom of 14 is written as byte `0x0E`, read back as a bad
conversion, and takes the whole layout down. `min_zoom`/`max_zoom` are
`uint16_t` for this reason.

**Build.** `Qt6::GuiPrivate` is what exposes `<rhi/qrhi.h>`; the `InitParams`
structs in `<rhi/qrhi_platform.h>` each sit behind their own backend's feature
test, and `QT_CONFIG(vulkan)` alone does not reach `QRhiVulkanInitParams` on
macOS, so mirror the header's own guard. `earcut.hpp` is included behind a
`-Wshadow` pragma rather than marked SYSTEM, which keeps `-Wshadow` live for
our code and the include out of Homebrew's `-isystem` order. This library does
not link `libs/mbtiles`, and its vendored `sqlite3` must never reach the
dashboard or editor, which link `Qt6::Sql`.

## Tests

Each unit test compiles the translation unit it is about rather than linking
the library, so it proves the dependency set its comment claims.
`map_test_projection` pins the Mercator anchors, rotation, the date line, the
stand-in walk and the tile budget with no Qt; `map_test_tessellator` the
triangles, joins and `roadPriority()`; `map_test_tile_cache` the eviction
policy with QtCore only; `map_test_tile_workers` the pool;
`map_test_buffer_arena` the suballocator. `map_test_gpu` (label `gui`) renders
headless and asserts that known geometry lands on known pixels, layer order
beats tile order, panning does not re-upload, hairlines never vanish, and a
road keeps its screen width under pitch.

`map_bench` is not a test: it asserts nothing and exits 0. It reads tiles
straight from an archive and reports the cost per stage; the number to watch is
`uploads`, which should climb only when the visible tile set changes.
`map_surface_bench` in `libs/map_surface` puts both hosts on a real screen.
Both are `EXCLUDE_FROM_ALL`, so `make` rebuilds the libraries and not them.

```bash
./build/libs/map_render/map_bench --tiles ~/Documents/map_data/socal.mbtiles
./build/libs/map_render/map_bench --tiles ... --width 2560 --height 1440 --dpr 2 --bearing 30
```
