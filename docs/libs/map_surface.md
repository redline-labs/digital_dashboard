---
title: map_surface
parent: Libraries
---

# map_surface

## Overview

The map drawn straight into a widget, with no readback. A separate target from
[map_render](map_render.html) for one reason: it is a `QWidget`, and
`map_render` deliberately is not. Everything that decides what a pixel looks
like stays in `map_render`'s `MapPass`, shared with the offscreen host; what is
here is one frame lifecycle, and it is here alone because this is the path that
cannot run under `QT_QPA_PLATFORM=offscreen`, so it must be small enough to
read rather than to test.

There are two ways to draw the map and they share nothing structurally. A
`QRhiWidget` records the pass into the widget's own swapchain, with no
readback, no blit, and no GUI-thread wait on the GPU: measured on a real
display, GUI-thread milliseconds per frame went from `3.09` to `0.27` at
660x640 and from `5.04` to `0.36` at 1920x1080 pitched. An offscreen QRhi
texture read back into a `QImage` and blitted by `QPainter` is slower and is
the only one that exists under `QT_QPA_PLATFORM=offscreen`, which is every
agent-control session and every `gui` test. Neither can be dropped, so the
choice is made once in the constructor and nothing outside this library sees
it. The `QRhiWidget` host is an addition to the offscreen host, not a
replacement.

## Public headers

| Header | Declares |
|---|---|
| `map_surface/map_surface.h` | `rhiWidgetsAreAvailable()`, and `MapSurface` with `setOverlayPainter()`, `setFrame()`, `setText()`, `refreshOverlay()`, `isUsable()`, `usesRhi()`, `backendName()`, the per-host timing counters, and the `Host` enum for forcing the offscreen path. |

The two hosts and the seam between them (`map_host.h`, `rhi_widget_host.h`,
`offscreen_host.h`, `map_overlay.h`) are under `src/` and are not public.

## Using it

Link the `map_surface` target, which brings `map_render` and `Qt6::Widgets`
with it. An embedder adds a `MapSurface` as a child under everything else,
hands it a frame, and hands it a callback for its own marker and trail. This is
`libs/dashboard_widgets/widgets/map/map_widget.cpp`; `apps/scope/panels/map`
is line for line the same.

```cpp
mSurface = new map_surface::MapSurface(this);
mSurface->setGeometry(rect());
mSurface->lower();
mSurface->setOverlayPainter([this](QPainter& painter) { paintOverlay(painter); });
if (!mSurface->isUsable()) { /* no GPU backend at all; say so */ }

// per frame
mSurface->setText(mTextQuads, mLabelCache.atlas().page(), mLabelCache.atlas().dirty());
mSurface->setFrame(projection, mBatches, mConfig.style, background, highlight);
```

## Behaviour worth knowing

**It cannot come up under `QT_QPA_PLATFORM=offscreen`.**
`QOffscreenIntegration::hasCapability` returns false for `RhiBasedRendering`
unconditionally, and a `QRhiWidget` bails before it picks a backend. The
constructor asks the platform once, through `Qt6::GuiPrivate`, and falls back
to the offscreen host. `rhiWidgetsAreAvailable()` is public so a diagnostic
overlay can say which path it got, not so anyone branches on it.
`MapSurface(Host::offscreen, parent)` forces the slow path on a machine that
has a display, for the bench and for tests that want both.

**Nothing in `src/rhi_widget_host.*` is exercised by any test in the tree.**
That is why it is short and why every pixel decision lives in `MapPass`. Read
it in one sitting; no test will say it drifted.

**The surface is `WA_TransparentForMouseEvents`.** Gestures land on the
embedder, as they did when it painted the map itself. Qt's hit test does not
descend into a transparent-for-mouse subtree, so floating chrome such as the
[map_controls](map_controls.html) buttons must stay children of the embedder,
not of the surface; a button parented in here draws and is never clickable.

**Overlay drawing goes through `setOverlayPainter()`.** A `QRhiWidget` has no
paint engine; `QPainter` on one logs `QWidget::paintEngine: Should no longer
be called` and draws nothing. The embedder's marker is painted on a transparent
child layer above the map, and `refreshOverlay()` repaints that layer without
redrawing the map, which with the `QRhiWidget` host costs no GPU work at all.

**Frames are copied.** With the `QRhiWidget` host the drawing happens when Qt
gets round to the frame, and by then the caller's projection and batch list are
gone; the copies are shared pointers and a few hundred ids, not geometry.

{: .important }
The headers are listed as sources in `CMakeLists.txt` because AUTOMOC does not
scan a header it has not been given. A `Q_OBJECT` class under `include/` that
is left out silently gets no metaobject and fails at link time far from the
cause.

## Tests

The library has no test target of its own; `libs/map_surface/tests/` is
empty. The offscreen host runs inside every `gui`-labelled test that hosts a
map, such as `map_test_widget` and `map_test_widget_hidpi` in
`libs/dashboard_widgets/widgets/map`, because that is the only host that
exists there. The `QRhiWidget` host has no automated coverage.

`map_surface_bench` is `EXCLUDE_FROM_ALL`, needs a display and a tile
archive, asserts nothing and always exits 0. It drives both hosts through the
same `MapSurface`, the same camera path and the same overlay in one process and
reports GUI-thread time per frame.

```
map_surface_bench --tiles ~/Documents/map_data/socal.mbtiles
map_surface_bench --tiles ... --width 1920 --height 1080 --pitch 55
```

{: .warning }
`make` rebuilds the libraries but not this binary. A stale bench will show you
last week's behaviour.
