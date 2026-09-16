---
title: qt_helpers
parent: Libraries
---

# qt_helpers

## Overview

Small Qt widget helpers shared by the GUI applications: a layered paint-caching
`QWidget` base, `helpers::Color` to `QColor`, and resource font loading. It is
the Qt-using sibling of [helpers](helpers.html), which is deliberately
Qt-free. These lived in the dashboard's include tree and nothing in them names
a dashboard; the scope app draws its plots on `CachedPaintWidget` and resolves
colours the same way.

Header-only. It links `helpers`, Qt6 Core, Gui and Widgets, and spdlog.

## Public headers

| Header | |
| --- | --- |
| `qt_helpers/cached_paint_widget.h` | `CachedPaintWidget`: static underlay and overlay pixmaps rendered on resize, dynamic content every frame. |
| `qt_helpers/widget_colors.h` | `toQColor()`: the one way to turn a configured colour into a `QColor`. |
| `qt_helpers/widget_fonts.h` | `loadResourceFont()`: a font from `:/` resources, memoized, returning its family name. |

## Using it

Link the CMake target `qt_helpers`. A gauge overrides the hooks it needs and
leaves `paintEvent` alone; every hook receives a painter with
`applyPaintTransform()` already applied:

```cpp
class MyGauge : public qt_helpers::CachedPaintWidget
{
  protected:
    void applyPaintTransform(QPainter& p) const override { gauge_paint::applyCenteredScale(p, *this); }
    void paintStaticUnderlay(QPainter& p) override { /* dial face and ticks, cached */ }
    void paintDynamic(QPainter& p) override { /* the needle, every frame */ }
};

const QString family = qt_helpers::loadResourceFont(":/fonts/futura.ttf");
painter.setPen(qt_helpers::toQColor(cfg.needle_color));
```

## Behaviour worth knowing

The cached layers are keyed on the widget size and the device pixel ratio, and
on nothing else. Without the DPR a window dragged between a Retina display and
an external monitor kept its old layers and drew them soft or oversharp.
Override `hasStaticOverlay()` to return true if you use the overlay hook, or it
is never rendered. `paintEvent` is `final`, and the class has no `Q_OBJECT`;
subclasses keep their own meta-object through `QWidget`.

{: .warning }
Only inherit this if your picture changes more often than it is repainted. The
cache earns its place on a gauge whose needle moves at 60 Hz over a dial face
that never changes: the face is drawn once, the needle every frame. A widget
whose whole picture depends on its own state gets nothing from it, because its
`paintEvent` fires exactly when that state changes. `mercedes_190e_telltales`
inherited it, drew everything in the static layer with an empty `paintDynamic`,
and had to throw the cache away on every change -- 10 microseconds of saving for
an API that existed to undo the caching. It is a plain `QWidget` now.

{: .warning }
`toQColor` splits `#RRGGBBAA` by hand. `helpers::Color` documents that form
with alpha last; Qt reads eight digits as `#AARRGGBB`, so handing it straight
over rotated every channel and produced a valid, wrong colour with no
complaint. Verified against Qt 6.10. Only that form is special-cased; everything
else goes through `QColor`, which accepts more spellings than
`isValidFormat()` does.

A colour Qt cannot parse falls back to the `fallback` argument (black by
default) with a warning. Config loading rejects malformed colours earlier, so
reaching that line means the colour arrived another way, such as the agent
interface or a live edit in the editor.

`loadResourceFont` is memoized per resource path. `QFontDatabase::
addApplicationFont()` re-parses the TrueType tables and returns a fresh id on
every call, so before the cache a dashboard parsed `futura.ttf` seven times at
startup and one widget re-registered it on every repaint. A missing resource
is cached as its fallback too, so the warning is logged once. The cache is a
function-local static and is only touched from the GUI thread.

The resources themselves are compiled into `dashboard_widgets`, under
`:/fonts/<file>` and `:/<widget>/<file>`; see
[dashboard_widgets](dashboard_widgets.html) for why they are added the way they
are.

## Tests

This directory registers no tests. `dashboard_widgets_test_config_colors`
(labels `dashboard_widgets unit`) pins `toQColor` by channel value, including
the eight-digit form, because parsing was never the failure.
