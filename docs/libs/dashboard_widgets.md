---
title: dashboard_widgets
parent: Libraries
---

# dashboard_widgets

## Overview

The widget set, and what it takes to build a layout out of it. Two
applications consume the same widgets: `apps/dashboard` renders a layout and
`apps/editor` edits one. Everything they need in common lives here so that
neither includes from the other's tree: the widget table and the registry
generated from it, the reflected layout config and its loader, the factory that
turns a config into a `QWidget`, the coalescing bus subscription every widget
reads through, and the `widget.*` agent methods, which sweep the widget table
and so cannot live in [agent_control](agent_control.html).

Every widget is its own static library under
`libs/dashboard_widgets/widgets/<name>` and registers itself through
`cmake/DashboardWidget.cmake`; this target links all of them, which is why they
cannot link it. The include prefix stays `dashboard/`, because these headers
describe a dashboard layout whoever is asking.

## Public headers

| Header | |
| --- | --- |
| `dashboard/widget_table.h` | `DASHBOARD_WIDGET_TABLE`, the one list of widget types. Includes nothing and must stay that way. |
| `dashboard/widget_types.h` | `widget_type_t`, generated from the table's first column plus `unknown`. |
| `dashboard/widget_registry.h` | Includes every widget header; `config_traits<>` from config type back to widget class; the `static_assert` that the enum matches the table. |
| `dashboard/app_config.h` | `widget_config_t`, `app_config_t` (one window), `dashboard_config_t` (the file), `load_dashboard_config()`, `validate_app_config()`, `default_widget_config()`. |
| `dashboard/widget_factory.h` | `createWidgetFromConfig()`: clamps through `validate()` and constructs. |
| `dashboard/widget_identity.h` | `widgetObjectName()` and `applyWidgetIdentity()`: the one naming rule both apps share. |
| `dashboard/expression_subscription.h` | `ExpressionSubscription<T>` and `makeExpressionSubscription()`: a bus value delivered to the GUI thread, coalesced. |
| `dashboard/widget_methods.h` | `registerWidgetMethods()` and `ConfigApplier`: `widget.describe_config`, `get_config`, `set_config`. |
| `dashboard/gauge_painting.h` | Shared needle, tick, label and range helpers for the analog gauges. |
| `dashboard/window_placement.h` | `display_role_t`, `scale_mode_t`, `WindowPlacement`, kept free of the widget table. |

## Using it

Link the CMake target `dashboard_widgets`; the widget libraries and the Qt
resources come with it. The dashboard's window loads a file and builds each
widget through the factory, naming it by the shared rule:

```cpp
#include "dashboard/app_config.h"
#include "dashboard/widget_factory.h"
#include "dashboard/widget_identity.h"

const std::optional<dashboard_config_t> config = load_dashboard_config(path);
for (std::size_t i = 0; i < window.widgets.size(); ++i)
{
    const widget_config_t& wc = window.widgets[i];
    QWidget* widget = widget_factory::createWidgetFromConfig(wc, this);
    dashboard::applyWidgetIdentity(widget, wc, i);
}
```

Inside a widget, data comes from the bus through one call; the setter runs on
the GUI thread:

```cpp
_expression_parser = dashboard::makeExpressionSubscription<double>(
    _cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
    this, &ValueReadoutWidget::setValue, "value readout");
```

Adding a widget is a five-step registration; [Adding a widget](../developing/adding-a-widget.html)
walks it, and the comment at the top of `widget_registry.h` is the canonical
version.

## Behaviour worth knowing

One table drives everything. The `widget_type_t` enumerators, the config
variant, `default_widget_config()`, the YAML decoder, the validator, the
editor's palette and the agent methods are all sweeps over
`DASHBOARD_WIDGET_TABLE`, so they cannot drift apart. A `static_assert` in
`widget_registry.h` fires if an enumerator is added to `widget_types.h` by
hand, which was the one route back to a widget that exists in the enum, cannot
be instantiated, and silently fails to parse. Table order fixes the variant
alternative order and the palette order; neither is persisted, so reordering
is safe but not a no-op. The 17 types are listed in `widget_table.h`.

{: .warning }
No `uint8_t` in a widget config. yaml-cpp writes it as a character, so 28
became a raw `0x1c` in the file and read back as a bad conversion that took
the whole layout down. Use `uint16_t`. `dashboard_widgets_test_config_roundtrip`
exists because of this.

A widget's `objectName` is its `id:` when set, otherwise `<type>#<index>`,
counted per window. The derived name shifts when widgets are added, removed or
reordered, so set an `id:` on anything worth addressing repeatedly. Both apps
apply the same rule, so a selector that finds a widget in the editor finds it
in the running dashboard.

The factory is the only construction path. A config may declare a free
function `validate(cfg)` (see [config_codec](config_codec.html)); the factory
finds it by ADL, clamps a copy of the config, logs each adjustment, and only
then constructs. There is no `instantiateWidget()` any more: it let the editor
preview a config the dashboard would have clamped.

`ExpressionSubscription` is a one-slot mailbox. The zenoh thread takes a short
mutex, stores the latest value, and wakes the one `DeliveryTicker` the process
has. Its next frame, at most 16 ms later, drains every subscription in a single
pass, so one publisher burst is one round of repaints; with nothing arriving no
timer runs. A value overwritten before the tick is never shown, which is the right thing
to do with a stale reading. The old shape posted one queued event per sample
with no bound, so a GUI that fell behind swept through a backlog while its
memory grew. It is not copyable or movable, because the callback captures
`this`; construct it through `makeExpressionSubscription()`, which returns
null with the error logged if the expression or the subscription failed.
`sinceLastSample()` is the raw measurement. The policy above it is
`stale_after_ms`, a field beside every binding's `zenoh_key`: after that long
with nothing arriving, `ExpressionSubscription::isStale()` turns true and the
subscription schedules a repaint -- the ticker keeps one timer set for the
earliest binding's deadline rather than polling, and both reverse when a
reading returns. Zero, the default, never goes stale, and the editor turns the
whole mechanism off for its process so a preview with no bus behind it does not
draw every widget as dead. A binding that cannot be built at all -- an
expression that does not compile -- goes stale the same way, because a gauge
showing zero for a broken binding is worse than one showing nothing.

The subscription is the only place the answer is kept. A widget holds its
subscriptions, so it asks one where it paints:

```cpp
bool valueStale() const { return _expression_parser && _expression_parser->isStale(); }
```

One subscription per binding means the bindings are independent for free: a
speedometer whose odometer topic has stopped still shows the speed. There is no
interface to implement, no flag on the widget to keep in step, and nothing to
mirror. The look is the widget's own, from one shared vocabulary in
`gauge_painting.h` (`kStaleColor`, `staleDashes()`, `drawNoDataLegend()`):
needles and markers go away rather than parking at zero, readouts show dashes,
and the sparkline stops scrolling. A warning lamp is the exception: the telltale
lights rather than greying, because a dark lamp says the condition is false and
that is the one answer a brake or battery lamp must not give when nothing has
reported.

The header includes `pub_sub/expression_subscriber.h`, the lean one, on purpose; a widget
that includes `zenoh_subscriber.h` pays for capnp in every translation unit.

Fonts and SVGs are added with `qt_add_resources`, not by listing a `.qrc` as a
source. A `.qrc` compiled into a static library is dropped at link time because
nothing references its initializer, and the failure is a wrong font and a
missing telltale rather than an error. The published paths are flatter than the
tree: `:/fonts/<file>` and `:/<widget>/<file>`.

`add_dashboard_widget()` links `reflection`, `helpers`, `config_codec`,
`qt_helpers`, `zenoh_pub_sub` and `Qt6::Widgets` PUBLIC, deliberately. A
widget's headers are part of its interface, and linking any of those PRIVATE
compiles the widget and breaks every consumer, since `app_config.h` includes
all of them. The failure never points at the cause: the helpers case was hit
three times and reports `helpers/color.h not found` against the including
file. A widget that needs something only in its `.cpp` passes `PRIVATE_LIBS`.

`registerWidgetMethods()` extracts and validates a config and hands the last
step to the app through a `ConfigApplier`, because the two apps rebuild a widget
differently. A null applier registers only the two read-only methods. In the
editor a selector usually lands on the `SelectionFrame`, so
`configBearingWidget()` looks one level down.

A config file is either a `windows:` list or, for one window, the flat form
every config used before there could be more than one. Both load;
`app_config_t` is one window and keeps its name for history.

`gauge_paint::clampToRange()` tolerates the two things a config can hand you
that `std::clamp` cannot: an inverted range, which is undefined behaviour, and
a NaN, which sails through both comparisons and into `painter.rotate()`. Every
gauge setter should go through it.

## Tests

The config tests are plain logic over reflected structs and need no display.
Their coverage is driven by the widget table, so a new widget is round-tripped
and validated without a new test.

| Target | Labels | Proves |
| --- | --- | --- |
| `dashboard_widgets_test_config_roundtrip` | `dashboard_widgets unit` | Every widget config survives being written to YAML and read back unchanged. |
| `dashboard_widgets_test_config_validation` | `dashboard_widgets unit` | `validate_app_config()` names the field in its message for each class of bad config the loader used to accept in silence. |
| `dashboard_widgets_test_config_colors` | `dashboard_widgets unit` | A configured colour turns into the channel values the file asked for, eight-digit form included. |
| `dashboard_widgets_test_staleness` | `dashboard_widgets unit` | The loss-of-comm state machine at its boundaries: disabled at zero, stale exactly at the timeout and not a tick before, one edge per transition, and the timeout measured from the newest sample. |
| `dashboard_widgets_test_subscription_stale` | `dashboard_widgets net gui` | Against a real publisher: no flicker on a stream arriving every 25 ms, stale once it stops, fresh again when it resumes, silent under the editor's suppression, and stale for a binding that could not be built. |

The `carplay` and `carplay_nav` widget directories register tests of their own
in their CMakeLists; their labels are not listed here.
