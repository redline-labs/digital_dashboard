---
title: map_controls
parent: Libraries
---

# map_controls

## Overview

The floating buttons both map surfaces overlay on their maps: a shared button
base, four glyphs, and the corner-stacking helper. It is a sibling of
[map_render](map_render.html) rather than part of it because these are
`QWidget`s and `map_render` deliberately holds none; it sits after
`map_render` in `libs/CMakeLists.txt` for reading order only, and links only
Qt. The dashboard's map widget and `scope`'s map panel both link it, and
neither may reach into the other's tree for a button again.

Everything here is chrome. The zoom, the recentre, the orientation cycle and
the view-mode toggle belong to the host; the buttons emit clicks, and the
compass emits a bearing while dragged.

## Public headers

| Header | Declares |
|---|---|
| `map_controls/map_button.h` | `MapButton`, the `QAbstractButton` base with `kSize`, `kMargin`, `kSpacing`, the `paintGlyph()` hook and `setEmphasis()`; `layOutStack()`. |
| `map_controls/recentre_button.h` | `RecentreButton`, shown only once the user has dragged the camera away. |
| `map_controls/compass_button.h` | `CompassButton` with `setBearing()` and the `bearingDragged(double)` signal. |
| `map_controls/view_mode_button.h` | `ViewModeButton` with `setPerspective()`. |
| `map_controls/zoom_button.h` | `ZoomButton` with `Direction::In` / `Direction::Out`; auto-repeats while held. |

## Using it

Link the `map_controls` target. Construct each button as a child of the
embedder with the style's label colours, connect `clicked`, and lay them out
again whenever the host resizes or a button shows or hides. This is
`libs/dashboard_widgets/widgets/map/map_widget.cpp`; `apps/scope/panels/map`
does the same with a different stacking order.

```cpp
const QColor glyph = qt_helpers::toQColor(mConfig.style.label_text);
const QColor disc = qt_helpers::toQColor(mConfig.style.label_halo);
mRecentre = new map_controls::RecentreButton(glyph, disc, this);
mCompass = new map_controls::CompassButton(glyph, disc, this);
mZoomIn = new map_controls::ZoomButton(map_controls::ZoomButton::Direction::In, glyph, disc, this);
connect(mRecentre, &QAbstractButton::clicked, this, &MapWidget::recentreCamera);
connect(mCompass, &map_controls::CompassButton::bearingDragged, this, &MapWidget::setManualBearing);

// on resize, and whenever a button shows or hides
map_controls::layOutStack({ mRecentre, mZoomOut, mZoomIn, mViewMode, mCompass },
                          QSize(width(), height()), Qt::BottomRightCorner);
```

## Behaviour worth knowing

These are real `QAbstractButton`s rather than rectangles painted into the
host's `paintEvent`, for three reasons that are each small and together
decisive: hover and press states come for free, the editor already makes child
widgets mouse-transparent in edit mode so a button cannot swallow a selection
drag, and each one appears in `ui_snapshot` as an addressable widget rather
than a coordinate the agent interface has to be told about. They paint
themselves because there is no icon pipeline in this tree.

The button takes the style's label text and halo colours rather than colours
of its own; that pair was already chosen to stay readable over water and
motorway alike. `kSize` is `34` logical pixels, sized for a finger on a
dashboard.

`layOutStack()` stacks nearest the corner first, `kSpacing` apart, and hidden
buttons take no slot, so a transient button leaves no hole; pass every button
and call again when one shows or hides. Both current users stack vertically;
horizontal is not implemented.

The compass needle draws at minus the camera bearing, which is where north is
on screen. Dragging it spins the map, with `bearingDragged` emitted per move
and nothing on release; confining rotation to the button is what keeps it from
fighting the pan gesture. When the map is effectively north-up the button
de-emphasises to `kDeEmphasized` rather than hiding, because it is the only
door into heading-up. The view-mode glyph shows the view a press would give,
not the current one; showing the current state reads as a broken toggle.

{: .important }
The buttons must be children of the embedder, not of the
[map_surface](map_surface.html) widget. The surface is transparent for mouse
events and Qt's hit test does not descend into it, so a button parented there
draws and is never clickable. The headers are listed as sources in
`CMakeLists.txt` for AUTOMOC; leave one out and `findChild<>()` on that type
fails to link.

## Tests

There is no test target in `libs/map_controls`. The buttons are exercised
through the `gui`-labelled tests of their two hosts, `map_test_widget` in
`libs/dashboard_widgets/widgets/map` and the map panel tests under
`apps/scope/tests`.
