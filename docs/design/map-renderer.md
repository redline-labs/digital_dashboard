---
title: Map renderer
parent: Design notes
---

# Map renderer

## Overview

These are the notes on why the map renderer in `libs/map_render` is ours rather
than an off-the-shelf Qt widget, what that decision cost and bought, how the
GPU path and the label pass came to be shaped as they are, and what a frame
costs on the hardware it was measured on. They are history and measurement,
not a manual. The manual pages are [map_render](../libs/map_render.html) for
someone coding against the library and [map_server](../nodes/map_server.html)
for someone running the node and configuring the widget.

Unless a section says otherwise, every number here was measured on an M-series
Mac with the Metal backend, against the SoCal archive at the dashboard's own
660×640. The section on Intel hardware is the one to read before trusting any
of them on the target.

## Why the renderer is ours

An off-the-shelf Qt map widget was the obvious choice, and one was built, wired
up and then removed. Two findings killed it.

They are `QRhiWidget` or `QOpenGLWidget` derived, and under
`QT_QPA_PLATFORM=offscreen` such a widget reports *"QRhi is not supported on
this platform"* and hands back a null map. Every `gui` test and every
`ui_screenshot` in this tree runs offscreen, so the map would have been
invisible to the entire agent-control loop, and the first symptom was a
segfault, not a blank image. The tree does now drive a `QRhiWidget`, for the
same speed those widgets promised, but only as one of two hosts behind
`libs/map_surface`, with the offscreen one still there for exactly this reason.
That is a different thing from depending on one.

It cost 7 GB of recursive submodules and 42 minutes of configure to find that
out, on every fresh checkout.

Worth being precise about, because two plausible readings are both wrong. It
is not that such a widget captures as black: measured on Qt 6.11/cocoa, a
`QRhiWidget`'s content comes back through `QWidget::grab()` and a child
composites over it correctly, which the Qt docs also state. And it is not
fixable by choosing a backend: the gate is
`QOffscreenIntegration::hasCapability` returning `RhiBasedRendering: false`,
which the Linux X11/GLX variant does not lift. The escape hatch is to stop
using the offscreen plugin, with Xvfb plus `xcb`, or `eglfs` on the target
board, which is how Qt GUI tests normally get a GPU in CI. It was not adopted:
there is no CI, `ctest` runs on developer machines, and macOS has no Xvfb.

A vector tile is protobuf with a command-encoded geometry stream. That turned
out to be a smaller thing to own than the consequences of not owning it. What
the replacement gives up is extruded 3D (see below) and GL style documents.
What it gains, beyond working offscreen, is that there are no glyph PBFs:
`QPainter` has fonts, so there is no font pipeline, no sprite sheet, and no
"the map has no labels and nothing said why" failure mode.

### The style is a reflected struct, not a style document

`MapStyle_t` is a colour, a width and a zoom threshold per kind of thing,
editable in the properties panel and patchable over `widget.set_config`. A GL
style is a small programming language, with filters, expressions and zoom
interpolation, and implementing it faithfully is a larger job than the renderer
it configures. The cost is that you cannot drop in someone else's style.

## How it draws

The geometry and the text are both on the GPU; where each one goes is decided
on the CPU.

A QPainter-only renderer was built first and measured at 241 ms per frame
(4 fps) on four z14 tiles, 14 440 features and 96 822 vertices, of which
buildings alone were 55 ms. That is a map you cannot pan.

The replacement is QRhi, driven two ways from one shared pass. What decides
what a pixel looks like is `map_render::MapPass`, which owns no target at all;
what varies is only the frame lifecycle, and there are two hosts for it.
`libs/map_surface` picks between them in its constructor, and nothing outside
that library learns which it got.

`RhiWidgetHost` is a `QRhiWidget`, recording straight into the widget's own
swapchain. No readback, no blit, and the GUI thread never waits for the GPU.
This is what runs on a real display.

`OffscreenHost` is `OffscreenRenderer` rendering into a texture it allocates
itself, read back into a `QImage` and blitted by a `QPainter`. Metal, Vulkan
and D3D render into a texture without any window at all, which is the one GPU
path that survives `QT_QPA_PLATFORM=offscreen`, so this is what agent control
and every `gui` test see.

Neither can be dropped, and the split is drawn where it is deliberately: the
fast path is the one CI can never exercise, so it is kept small enough to read
in one sitting (about 143 lines) while about 880 lines of pass are shared
verbatim.

Measured on a real display, GUI-thread milliseconds for one map frame with both
loops paced identically. The readback and blit are ten to twenty-five times the
cost of recording the same drawing.

| | offscreen | QRhiWidget |
|---|---|---|
| labels + batches *(identical work; the control)* | 0.38 | 0.38 |
| the map frame | 2.06 | 0.08 |
| overlay (marker + trail) | 1.94 | 1.92 |

At 1920×1080 pitched the map frame is 6.25 ms against 0.24 ms. Measure with
`map_surface_bench --tiles ... --width ... --pitch ...`, which needs a screen.

Because a `QRhiWidget` has no paint engine (`QPainter` on one logs
*"QWidget::paintEngine: Should no longer be called"* and draws nothing), a
host's marker and diagnostics cannot be painted after a blit any more. They go
through `MapSurface::setOverlayPainter()` onto a transparent child layer above
the map, which Qt composites; the same layer is used with both hosts so there
is only ever one overlay path. Measured: the layer itself costs 3 us, and a
sibling widget over the nested render-to-texture widget composites correctly
and is still found by `childAt()`, which is why the floating buttons stay
children of the host.

Measured on the same scene: tessellation 1.9 ms once, upload 6 ms once, and
then a fraction of a millisecond per frame with pan, zoom and rotate.

The offscreen frame is linear in pixels, not flat: about 0.5 ms per megapixel
of the target, so 660×640 is about 0.4 ms and the same widget at a device pixel
ratio of 2 is about 1.0 ms; a 2560×1440 one goes from about 1.8 ms to about
7.4 ms at 2x. The cost is the readback rather than the rasterisation: forcing
`sampleCount` to 1 moves a 5120×2880 frame by under a millisecond, which is why
the sample count is taken as high as the hardware offers and the size is the
thing to think about. Measure with `map_bench --width/--height/--dpr`. None of
this applies to the `QRhiWidget` host, which neither reads back nor blits; that
is the whole reason it exists.

There is no CPU fallback: without a backend the widget draws its background
and says so, which `status().gpuReady` reports.

### Labels: placement on the CPU, drawing on the GPU

Which labels survive depends on what else is already on screen, across tiles,
so collision is viewport-global and cannot be baked into the per-tile geometry
the GPU caches. That argument still holds and `labels.h` still carries it.
Placement and collision cost 0.12 ms a frame.

Drawing them was the other 2.33 ms, 95% of the pass, and that is now a vertex
buffer of textured quads sampled from a glyph atlas, drawn last inside the same
offscreen pass as the tiles. Measured: +0.07 ms on the GPU, one extra draw
call, and the whole frame fell from 3.15 ms to 0.93 ms.

The atlas holds what QPainter rasterised, so this is a transport change rather
than a second text renderer. QPainter is still the font stack; there are still
no glyph PBFs, no font pipeline, and no way for the map to come up with no
labels and nothing saying why. Two consequences follow. Every character is
placed individually, always: the old code drew a straight label as one blit and
only a bent one glyph by glyph, a split that existed solely because a rotated
CPU blit costs about 4 us, and a quad costs nothing worth splitting for, so the
whole-string image cache is gone. And text is part of the frame's identity: two
frames can agree on every tile and still be different pictures, so the quads
are compared by the memo alongside the tile serials. Without that a parked
vehicle would hold a stale frame whose labels had moved.

## 3D: a pitched camera, not extruded geometry

The renderer draws a tilted flat map. `Camera` has `center`, `zoom`, `bearing`
and `pitch`. Pitch is degrees off straight-down, capped at `kMaxPitch = 60`,
and that cap is what keeps the whole thing simple: at 60° the horizon line
still sits above the top edge of the viewport (0.866 viewport-heights above
centre versus the edge's 0.5, at the 1.5×-height focal length), so every screen
pixel intersects the ground plane and `screenFor()`/`worldForScreen()` stay
total functions with exact closed-form inverses. No sky, no fallible
projection, no horizon clipping.

**No depth buffer, still.** Everything drawn is coplanar on the ground, so
layer-major painter's order stays correct at any camera angle. `map.vert` was
already projective (`gl_Position = mvp * vec4(p, 0, 1)`) and pitch makes `w`
finally vary. The tilt is one matrix per frame, composed in device space
between `screenToClip()` and the flat per-tile placement; `tileOrigin()`
deliberately stays flat (rotation, no tilt) so nothing tilts twice. See the
header note on it in `projection.h`.

**Per-tile LOD.** Under pitch `visibleTilesWithMargin()` switches from the
single-zoom grid walk to a quadtree descent: prune tiles whose flat-screen quad
(an undistorted rotated square, the one space where both shapes are exact and
convex) misses the viewport trapezoid, split while a tile would draw wider than
`tileSize·√2` at its nearest visible corner, emit the leaf otherwise. Leaves
partition the viewport; a 60° frame is a few dozen tiles, not hundreds. The DFS
order is a pure function of the emitted set, which is what the renderer's
positional batch comparison needs. `minLeafZoom` floors the far field at the
archive's own minimum. The overview-ring request is skipped under pitch; the
descent's far-field leaves are the coarse levels.

**Line widths are expanded after the matrix**, in clip space.
`expandToScreenWidth()` in `map.vert` projects the centreline, projects a short
step along the normal, and offsets by the half-width along that direction
measured in pixels. A road is then exactly as many screen pixels wide as the
style asked for, wherever it is and whatever zoom level its tile came from.
Expanding in tile-local space and dividing by a per-tile scale, which is what
this did first and what the flat map got away with for years, cannot work under
perspective. The local-to-screen scale is not constant across a tile (it falls
off with distance) and is not the same in x and y (the vertical carries
`cos(pitch)` and a second factor of `w`), so one scalar per tile leaves a road
tapering across each tile and stepping at every tile boundary. The step is
worst where a coarse far-field tile meets a fine one: a big tile normalised
about a centre far off-screen draws the sliver you can actually see roughly
twice too wide. Measured before the change, at 55 degrees: a road of a flat
30 px ran 15 px at the far edge of its tile and 29 px at the near edge.
`test_a_road_keeps_its_screen_width_under_pitch` holds it to within 15% of the
flat width down the frame, and to within 2 px across a LOD boundary. It costs
one extra matrix multiply per line vertex (fills carry a zero normal and skip
it), about 0.2 ms a frame on the bench's pitched view. At pitch 0 the
arithmetic reduces to what it replaced, so the flat map is unchanged.

**Labels.** `Projection::TileTransform` carries the tilt behind one early-out
branch (the flat frame still pays a single predictable compare) and grew
`scaleAt(lx, ly)`, the tilt's shrink at a point. The label pass scales glyphs,
advances and collision claims by the anchor's scale, clamped to [0.5, 1]; below
half size the name is dropped rather than smudged onto the horizon. Duplicates
across zoom levels only arrive via stand-in tiles, and the same world point
lands on the same pixel, so collision already rejects the copy.

Buildings are still drawn as footprints, not extrusions. Extrusions are the
separate, additive project, the one a depth buffer is actually for, and also
need `render_height` carried through `map_build` into the archive.

Measured (`map_bench --pitch`, socal archive, 660×640): whole frame 0.94 ms
flat, 1.39 ms at 60° with bearing 30. The increase tracks the larger visible
footprint (tiles, labels), not any per-pixel regression, and the pitch-0 path
is bit-identical to the old projection (asserted in `test_projection.cpp`).

### View modes on both surfaces

Both surfaces expose it: the dashboard widget as `view_mode`
(top_down/perspective) plus `pitch` and `orientation` (north_up/heading_up),
the scope panel with `course_up` instead of heading_up. The panel has no
vehicle heading, so its analogue is course over ground at the shared cursor,
smoothed over ±3 track points. The buttons that toggle these live in
`libs/map_controls`.

The corner buttons are real `QAbstractButton`s rather than rectangles painted
into `paintEvent`, which buys hover and press states, makes the editor's
recursive mouse-transparency (`Canvas::setEditorMode`) cover them in edit mode,
and puts them in `ui_snapshot` as addressable widgets (`mapRecentreButton`,
`mapCompassButton`, `mapViewModeButton`) instead of coordinates the agent
interface has to be told about. The shadow is hand-painted rather than a
`QGraphicsDropShadowEffect`, which misbehaves under the offscreen platform.

Each button carries one decision worth recording. Recentre drops the pan and,
in the widget, does not touch the zoom: the user chose that separately, and
asking to be recentred is not asking to be zoomed back out. In the scope panel
it clears the wheel zoom too, because there the wheel breaks Follow Cursor and
coming back means coming all the way back. The compass is both the orientation
toggle and the rotation control: its needle points at true north on screen,
fed the frame's actual bearing from the paint pass, so in heading-up it swings
with the vehicle. Dragging the needle spins the map, with the bearing following
the cursor's angle about the button centre so the needle stays under the
finger; confining rotation to the button keeps it from ever fighting the pan
gesture, and it works the same for mouse and touch. A drag forces manual
control (north-up plus a session bearing override) whatever mode was driving
the bearing before, and a plain click straightens first: a manually spun map
snaps back to the configured bearing, and only a click on an unspun map cycles
the mode. When the map is effectively north-up the compass de-emphasises rather
than hiding, because it is the only door into heading-up and must stay
pressable. The view-mode glyph shows the view a press would give, a trapezoid
while flat and a flat square while tilted, because showing the current state
reads as a broken toggle. The zoom pair steps one level per press through the
same door the wheel uses, eased and centre-anchored on the dashboard so Follow
Vehicle survives exactly as it survives a wheel notch, and auto-repeats.

The toggles and the manual bearing are session state, mirroring the
interaction optionals: the config's `orientation`/`view_mode`/`bearing` fields
are what the layout or workspace opens with, and a button press never writes
back to them. The dashboard stacks its buttons bottom-right; the scope panel
top-right, because `paintLegend()` owns its bottom-right corner. On a host too
short for the whole stack, `layOutStack()` sheds buttons from the far end; each
caller orders its list so the least essential go first.

## Panning, zooming and the camera

Both gestures go through `Projection::worldForScreen()`, which is what makes
them anchored rather than approximate: a drag keeps the point you grabbed under
the pointer, and the wheel keeps the point under the pointer where it is while
the scale changes. Anchoring in world units also means it stays correct under
rotation; a screen-space delta sends a bearing-rotated map off at an angle to
the drag.

The camera has three sources, in order: where the user dragged to, then the
vehicle if `follow_vehicle` is on, then the configured centre. A drag suspends
Follow Vehicle, and having a dragged-to centre *is* what suspended means; there
is no second flag to keep in step with it. The alternative is a map that snaps
back on the next position fix and cannot be looked away from at all. The wheel
does not suspend it: while Follow Vehicle is on, zooming anchors on the centre
instead of the pointer, so the vehicle does not move on screen and there is
nothing to suspend. This is deliberately not conditioned on a position having
arrived. Following is a declared intent, not a state that waits for a fix, and
a zoom taken while the GPS is still coming up must not cancel it for good. The
panned centre is clamped to the Mercator limit and wrapped at the date line, so
a drag cannot leave the projection.

`map_test_widget_hidpi` aside, an offscreen widget has no screen and so no
mouse; the interaction tests post `QMouseEvent`/`QWheelEvent` straight at the
widget, which reaches the same handlers Qt would.

## Labels

Two kinds, placed by one pass so they compete for the same screen space.

**Point labels** come from the basemap's `place` layer and the tracks archive's
`track_label`. They are ranked by the `rank` and `population` attributes
`map_build` writes rather than by the class name alone: the class cannot tell a
town of 400 000 from one of 400, and where the city/town line falls is local
convention that moves by an order of magnitude between countries. A place big
enough is promoted a tier, using the same thresholds `map_rules` already uses
to promote its minZoom; promoting the zoom and not the rank is what drew a
large town from z6 and then labelled it under a city of 3 000. Promotion is
upward only, because a city tagged with a small population is far more often a
stale tag than a tiny city. An archive with no `rank` falls back to the class
name, so the bench archive still labels.

**Road names** come from `transportation_name`, and are the one line-placed
layer. They are laid along the road, at the middle of the longest visible run.
The longest run rather than the first, because MVT clips a road into as many
parts as it has crossings of the tile edge and the first is as often as not a
stub in a corner; the pieces one way was clipped into are merged back together
before "longest" is decided, so a road that leaves the tile and returns is one
run rather than two fragments. Following the road means the run's geometry has
to survive to the paint pass, which is why `LabelCandidate` carries a range
into a flat arena of tile-local points rather than only an anchor. A name is
repeated along its road but never twice in the same place (see below), and a
name wider than the road it names is dropped, which is what stops the
two-metre stubs MVT leaves in tile corners from each claiming a label.

They rank between a neighbourhood and a locality: a street name must not push a
town off the map, but at the zooms it appears at, the street the driver is on
is worth more than a junction three miles away. Among themselves they order by
`roadPriority()`, the tessellator's own ladder, reused rather than restated so
the layer a road is drawn in and the weight its name carries cannot drift
apart. `detail.road_label` is the zoom floor, z14 by default, and
`show_road_labels` turns them off outright.

### Curved road labels

A road name is laid out character by character along the road, each glyph
rotated to the road's bearing where it sits. Three rules govern it.

Always upright. The direction is decided once per label, from where the first
and last characters land: if the name would read right-to-left across the
screen, the same run is walked from the other end and every glyph turned 180°.
Never per glyph, because flipping glyphs individually scrambles the word. This
is what lets road labels rotate at all without breaking the rule the rest of
the pass keeps: place names still never rotate, at any bearing.

Rejected where the road kinks. The sum of the turn between consecutive
characters, over any window of about 0.6 em of road, may not exceed 45°
(MapLibre's `text-max-angle` default, and its reasoning). A sliding window
rather than a per-glyph limit is the whole trick: a per-glyph threshold passes
a spiral made of shallow steps and rejects one kink in an otherwise straight
road.

Collision by several boxes, not one. The axis-aligned hull of a diagonal word
is mostly empty space and evicts neighbours it never touched, so a curved label
claims one box per four characters. Straight labels still claim a single
rotated box.

`kStraightEnoughPx` picks the shape of a label's collision footprint: one
turned rectangle for a straight name, a chain of hulls for a curved one. It
used to decide much more. On the CPU a straight name was one blit and a curved
one a rotated blit per character, worth roughly fifteen-fold, but on the GPU
every character is a quad either way. It is asked in pixels of deviation from
the chord, not in degrees, because the question is "would drawing this as one
straight image look different?" An angle threshold gets it wrong in both
directions: half a degree is invisible on a short name and obvious on a long
one, and real roads wander by fractions of a degree constantly without looking
bent. Asked in degrees at half a degree, two thirds of the road labels in
Irvine, a grid city, came out "curved".

Measured on the SoCal archive at 660x640, z14, M-series, when the labels were
still drawn by QPainter:

| | before | after |
|---|---|---|
| labels | 0.58 ms | 2.31 ms |
| whole frame | 1.34 ms | 3.1 ms |

24 labels placed, of which 13 genuinely curve. Bearing makes almost no
difference (a rotated straight road is still one blit), which is why
`map_bench` grew a `--bearing` flag but the number it reports barely moves.

The halo must be a separate pass over the whole label. Each glyph is cached as
two images and every halo is laid down before any fill. One combined sprite per
glyph would let the next character's opaque halo paint over the previous
character's text wherever they kern tightly, and the word comes out gnawed.

### Repeating a name along its road

A long road carries its name several times, so it is legible wherever the
driver happens to be looking rather than only at a midpoint that may be off
screen. `style.label_repeat_distance` (250 px) is the clear road demanded
between one instance and the next; 0 restores one label per name per viewport,
which is what the map did before repeats existed.

The dedup rule changed shape to allow this. It used to be "this name is already
on screen, drop it"; it is now "this name is already on screen *within
label_repeat_distance of here*, drop it". The old rule is the new one with the
distance at infinity, which is why zero still means once per viewport. Without
any dedup a street is labelled once per tile it crosses, the failure this has
always existed to prevent.

Repeats are generated per run and centred on it, so the single-label case is
exactly the old placement rather than an approximation of it.
`kMaxRepeatsPerRun` (16) bounds a short name on a long run with the distance
turned down; without it one feature could claim an unbounded number of
collision boxes, and the per-frame scan is quadratic in those.

Still not done: the phase-offset trick MapLibre uses to keep repeats at the
same absolute positions across a parent tile and its overscaled children. Our
dedup is viewport-global rather than per tile, so the symptom that trick exists
to prevent does not arise here.

### Water names

`water_name` is the one layer that carries both shapes: a lake's name sits at a
point inside it, a river's runs along the line. `map_build` emits them that way
deliberately (`extract.cpp`), because "a point would put 'Santa Ana River' in
one spot on a watercourse forty miles long", so `LabelGeometry::Either` follows
the feature rather than the layer. Rivers get the same curved treatment and the
same repeats as roads; lakes are placed and drawn like any other point label,
upright at every bearing.

Water ranks below roads, and below a locality with them. On a map for driving,
the street you are on outranks the river you are crossing, and a river is a
long feature that would otherwise repeat its way across the viewport at a
road's expense. Among themselves they order by size and permanence: sea, lake,
river, canal, stream, ditch.

`detail.water_label` is the zoom floor (z14, matching the archive's own) and
`show_water_labels` is the toggle. They are separate from the road pair on
purpose; turning street names off leaves the rivers named.

A nameless river is not a bug. `map_build` emits river segments with no name
because the layer carries the geometry a label runs along, and the name may
live on a parent way that was split. Extraction drops them, as it drops any
feature with no text.

## Not blanking while tiles are in flight

Drawing only what has arrived is what makes a map flash its background on every
zoom, and it reads as a fault rather than as loading. The data to avoid it is
usually already in hand: `TileSource`'s cache is keyed by `TileId` across zooms
and is never cleared on a camera change, so zooming in leaves the shallower
tiles you came from and zooming out leaves the deeper ones. And a tile drawn at
a zoom other than its own needs no special handling at all; `tileOrigin()` and
`tileScreenSize()` place it from its own id.

So each paint, `substituteTiles()` asks the cache what can go under the gaps.
Ancestors first, nearest wins: one covers a tile and its three siblings, so
four missing tiles usually cost one extra draw call between them. Measured
against the real archive, six missing z12 tiles were covered by two z11
ancestors. Then one level of descendants. They partition their parent, so
unlike an ancestor they overlay nothing. This is the zoom-out case, and one
level is the cap because two is sixteen tiles for one; a zoom gesture moves a
level at a time anyway.

Stand-ins go first in the batch list, so within each layer pass a real tile
covers its own ground. Where an ancestor spans a tile that did arrive they
overdraw, which is harmless: an ancestor is the same geography drawn with less
detail, so the two coincide.

Stand-ins label too, but only after every real tile, and that order is the
whole trick. Leaving them out of the label pass, the first thing tried, made
the text blink out for the frames a zoom was in flight while the geometry
underneath stayed, which is a worse artefact than the blank map this was meant
to fix. Duplicates take care of themselves: a place named by both an ancestor
and the real tile lands on the same pixels, and `layOutText()` rejects a
candidate colliding with one already placed, so real tiles going in first
decides which wins. Measured through a z11→z12 transition: 18 labels during, 18
after, in the same positions.

The diagnostic caption knows about them. `paintDiagnostic()` explains a map
with nothing on it, so it tests `tilesDrawn` and `tilesStandIn`; without the
second it captions a perfectly good stand-in frame with "No coverage here in
tileset 'socal'", which is a line of text flashing over visible roads.

It is budgeted. The renderer draws at most `kMaxTilesPerFrame` and truncates
the tail, where the real tiles are, so stand-ins are capped at what is left
over. `status().tilesStandIn` reports how many were used; a number that stays
non-zero means tiles are not arriving.

Panning at a constant zoom is not covered, and cannot be by this mechanism: the
leading edge's ancestor is only cached if you happened to view that ground at a
shallower zoom. The one-tile prefetch ring covers a pan of about a tile; a
coarse overview cover four levels down is requested alongside it, at the end of
the request list so it can only spend slots the viewport did not want, and that
is what covers ground never visited at any zoom. Four levels because one
overview tile spans 16x16 of the drawn ones, and because
`kMaxSubstituteLevelsUp` is five; an overview deeper than the substitute walk
looks would be fetched and never drawn.

Zooming in past the archive never produces a gap at all, for a different
reason: `tileZoom()` clamps to what the archive holds, so the tiles are already
the ones on screen, magnified.

## Who decides the zoom range

Two different questions, and the widget stopped conflating them.

| | who answers | what it means |
|---|---|---|
| which tile level to fetch | the server, on every reply | what the archive actually holds |
| how far the camera may zoom | `min_zoom`/`max_zoom` in the layout | how close a layout lets the user get |

`MapTileResponse` carries `minzoom`/`maxzoom` on every reply, `TileSource`
keeps the first pair it sees, and `refreshTiles()` clamps `tileZoom()` to it.
So a layout no longer transcribes a number that has to match an `.mbtiles` file
on another machine; point a widget at a deeper archive and it uses the depth.

Why the reply carries it rather than a catalogue call, and rather than nothing
at all: a client cannot infer the depth from the per-tile answers. Most of the
pyramid is empty, so a hole over the desert at z14 and a level the archive
never had both come back absent. Left to work it out, a client wanting to zoom
past the archive would walk down a level at a time, one round trip each, and
would still walk all the way to zero over genuinely uncovered ground. Two bytes
on a reply that already carries a hundred kilobytes of tile removes all of
that, and the first reply of a session is enough.

`checkTileRange()` in `tilesets.h` separates the three answers a tile request
can get short of a tile, and `map_server_test_tile_range` pins them. Getting
`outOfRange` and `notFound` the wrong way round fails quietly in both
directions: a blank map the moment anyone zooms past the archive, or a client
retrying uncovered ground a level up, and again, all the way to zero, once per
tile. `badRequest` is screened before anything shifts by `z`, because
`1U << 40` is undefined rather than large.

Overzoom is cheap; underzoom is not. A camera deeper than the archive looks at
a fraction of one tile, free, and magnified vector tiles stay sharp, because
the geometry is triangles in tile-local coordinates and roads are expanded in
the shader to a screen-pixel width. Measured against the SoCal archive (z14):
z16 is indistinguishable in sharpness, z17 still reads as streets, and by z18
there is too little left in frame to be a map, hence a default `max_zoom` of
17. A camera shallower than `minzoom` is the opposite: 256× the tiles at four
levels out, truncated by `kMaxVisibleTiles` into a partly drawn map.

Learning the range has to trigger a repaint. When the camera is parked past the
archive, every tile comes back `outOfRange`, nothing lands in the mailbox,
`drain()` returns zero, and without `TileSource::takeArchiveRangeLearned()` the
widget would sit blank holding a range it never redrew with. That bug was real;
it is why the flag exists.

## The tile cache, and what leaves it

`map_render/tile_cache.h`, split out of `TileSource` because that class cannot
be built without a zenoh session and the eviction policy is the part worth
testing.

It is bounded by both a tile count (256) and a byte budget (128 MB), because
neither works alone: an empty ocean tile is a few dozen bytes and a downtown
z14 tile is a few hundred kilobytes, so a count alone permits anywhere between
nothing and hundreds of megabytes depending on where the drive went, while a
byte budget alone would let a city of tiny tiles grow the map without limit.

Least recently used, and asking counts as use. `ready()` and `drawable()`
promote what they are asked about, which is every tile the paint pass looks at.
Insertion order, which this was, evicts the tile under the vehicle in favour of
one the drive left behind an hour ago, and an evicted tile that is still on
screen is re-fetched immediately: the cost is a round trip and a
re-tessellation every frame rather than once. `contains()` is the exception and
deliberately does not promote: `request()` asks it of the whole viewport plus
its prefetch ring on every paint, and treating that as use would protect
exactly the speculative ring tiles that should go first.

The floor wins over the byte budget. Eviction stops at 80 tiles however heavy
they are, because a viewport bigger than the budget would otherwise evict tiles
it is still drawing and re-request them on the next paint, a refetch loop that
reads as a slow server rather than as a small cache. With worst-case tiles that
knowingly exceeds the budget. Memory spent holding the screen is worth more
than memory saved thrashing it.

## What a frame costs away from Apple silicon

The numbers elsewhere in this page were measured on an M-series Metal backend.
On a Linux box with an Intel UHD 630 through Mesa, measured with `map_bench`
against the real SoCal archive at the dashboard's own 660x640:

| | this box | M-series (above) |
|---|---|---|
| gpu render | 4.83 ms | ~0.4 ms |
| labels | 2.79 ms | — |
| whole frame | 8.43 ms | — |

Those figures predate both curved labels and the glyph atlas. On the M-series
box the same frame is now 0.93 ms with text on the GPU, against 3.15 ms with it
on the CPU; nothing has re-measured the Mesa box.

The GPU frame is about twelve times slower, and it is the readback, exactly as
the rest of this page warns. Fitted across three viewport sizes, 0.11, 0.42 and
1.69 megapixels, it comes to:

    gpu_frame = 2.44 ms fixed + 3.51 ms per megapixel

Two things separate that from Apple silicon. The per-megapixel term is seven
times the 0.5 ms/Mpx measured on Metal, because `readBackTexture` there is a
copy inside unified memory while here it crosses the bus from the iGPU. And
there is a 2.44 ms fixed cost far larger than Metal's, which is the synchronous
`endOffscreenFrame()` pipeline stall: cheap on a tiler with shared memory,
expensive on Mesa.

The consequence inverts the advice above: on this hardware the fixed cost
dominates at dashboard sizes, so making the viewport smaller buys much less
than the M-series numbers suggest. Halving the linear dimensions took 4.83 ms
to 2.08 ms, not to a quarter.

This is the box the design should be judged on, not the Mac. Since text moved
to the GPU the label pass is about 0.15 ms everywhere, so on Intel the frame is
now almost entirely the GPU stage, and that stage is almost entirely waiting
and copying rather than drawing. Two numbers that do not transfer from Apple
silicon: on unified memory the readback is about 0.15 ms of a 0.88 ms stage,
where on the UHD 630 it is about 1.5 ms at 660x640 and grows at 3.51 ms/Mpx;
and the synchronous stall is about 0.73 ms on Metal against 2.44 ms here. So
the roughly 0.9 ms frame measured on the M-series is likely nearer 5 ms on an
Intel-class iGPU, essentially all of it in `endOffscreenFrame()`.

### What that wait is, measured

Not idle. Isolating the steady-state loop (2200 frames minus 200, so 2000
frames of rendering with startup subtracted) on the M-series box:

| | marginal, 2000 frames |
|---|---|
| wall | 1.28 s |
| user CPU | 1.35 s |
| sys CPU | 0.37 s |
| total CPU | 1.72 s, i.e. 134% of wall |

If `endOffscreenFrame()` slept on a fence, CPU time would be well under wall
time: the GPU stage is 61% of the frame, so around 40% would be expected. It is
134%, with 29% of wall in the kernel. So the stall is a spin plus driver
threads doing real work, and it costs about a core.

{: .warning }
This is measured on Metal/unified memory and may not hold on Mesa. A 2.44 ms
fixed cost there could equally be a blocking wait, in which case the trade
below looks quite different. The same `/usr/bin/time -p` comparison on the
target answers it in two minutes and is worth doing first.

### What pipelining would and would not fix

Render frame N on a thread of its own and blit whatever finished, so the wait
overlaps the rest of the frame instead of sitting in front of it. Prototyped
and measured at -29% here, where the stall is only 0.73 ms; worth
proportionally more wherever it is 2.44 ms.

Be clear about what it changes, because "it fixes the spin" is the wrong
reading. Latency improves; this is the entire point. CPU is relocated, not
removed: the spin still burns a core, but on the worker thread where it no
longer blocks label placement, so total CPU per frame is roughly unchanged,
instantaneous utilisation goes up and wall time goes down. Removing the spin
outright needs a non-blocking readback, which the offscreen-frame API does not
offer; that is a swapchain, i.e. QRhiWidget, i.e. the second render path. GPU
work is unchanged: same commands, same work. Memory bandwidth is slightly
worse, and this is the one to watch on a low-power part. The prototype copied
the frame (1.7 MB at 660x640, about 100 MB/s at 60 Hz) to hand the GUI thread a
stable image. That is avoidable: give the worker two readback buffers and swap
pointers rather than copying.

The cost is one frame of latency on the tile raster, which the marker and the
text do not share; they are placed at the current camera every frame.

Labels are fine. 2.79 ms for about 450 candidates and about 25 placed, with the
render cache hitting every time after the first frame. A single label costs
1.27 ms to stroke and fill here against the 0.88 ms recorded on macOS, so the
raster engine is about 1.4x slower and nothing more. Those figures predate
curved road labels, which cost about 1.7 ms more on the M-series box. Nothing
has re-measured them on this hardware; if the 1.4x holds, expect the label
stage nearer 5 ms here.

### The gpu render stage measures the CPU's state as much as the GPU's

`endOffscreenFrame()` is a synchronous round trip, and how long it takes
depends on what the CPU was doing immediately before it. Measured, with the GPU work
held identical and only unrelated CPU work varying between frames:

| CPU between frames | gpu render |
|---|---|
| idle | 0.37 ms |
| ~2 ms of work | 0.57 ms |

Interleaved over four pairs; the busy figure is stable to 0.006 ms while the
idle one wanders between 0.36 and 0.59 ms, which is what an idle core dropping
into a low-power state looks like. So moving text off the CPU made the same GPU
work measure faster, and a stage-by-stage comparison across a change that
alters CPU load is not apples to apples. Trust whole frame; it is both the
number that matters and much the steadier of the two.

{: .warning }
`map_bench` is CPU-bound in the label pass and GPU-bound in the readback, so
two of them running at once report each other's contention rather than the
frame's cost. Concurrent runs on the Mesa box produced a label figure of
476 ms, a hundred and seventy times the real one, and it looked entirely
plausible. Run it alone, and check `uptime` first.
