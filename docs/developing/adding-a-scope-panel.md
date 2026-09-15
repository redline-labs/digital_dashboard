---
title: Adding a scope panel
parent: Developing
nav_order: 3
---

# Adding a scope panel

A scope panel is a view onto one or more bound signals: the time-series plot,
the table, the video panel and the map panel are the four that exist. Adding one
is three steps, and the comment at the top of
`apps/scope/include/scope/panel_registry.h` is the canonical statement of them.
The arrangement is the same as the dashboard's widget table, for the same
reason: a list repeated in several places will disagree with itself.

## The panel class

The panel lives under `apps/scope/panels/<name>/`, derives from `scope::Panel`,
and exposes:

```cpp
using config_t = MyPanelConfig_t;      // REFLECT_STRUCT, in its own config.h
using stats_t  = MyPanelStats_t;       // REFLECT_STRUCT, in its own stats.h
static constexpr panel_type_t kPanelType = panel_type_t::my_panel;
static constexpr std::string_view kFriendlyName = "My panel";
static constexpr std::string_view kToolbarGlyph = "M";   // one or two characters
const config_t& getConfig() const;
void applyConfig(const config_t&);
stats_t stats() const;
```

`acceptsBinding()` on `scope::Panel` is what decides which signal-browser
candidates the panel will take, and `bindingLabels()` / `removeBinding()` hand
bindings back, so the browser, the drag, the context menu and the agent
interface need no knowledge of panel types.

{: .important }
The stats struct is not optional. `scope.stats` and `scope.describe_stats` are
served by visiting it, and a panel without one would answer `{}`, which looks
exactly like a working panel that has received nothing. Put in it what would
tell you the panel is lying: counts of what arrived and what was dropped, not
what a screenshot already shows. A panel with nothing to report declares an
empty struct.

The toolbar glyph is a character rather than an icon because scope has no icon
pipeline at all; swapping glyphs for icons later is a change to that field and
to the toolbar, and to nothing else.

## Registering it

1. Add one line to `SCOPE_PANEL_TABLE` in `apps/scope/include/scope/panel_table.h`.
2. Add one `scope_add_panel(<dir> <sources...>)` call in
   `apps/scope/CMakeLists.txt`, listing any `Q_OBJECT` header among the
   sources. Leaving one out fails as a missing `staticMetaObject` at link time.
3. Include the panel's header in `panel_registry.h`.

Everything else follows: the enum, the config variant, `default_panel_config()`,
the Panels menu, the workspace YAML codec, and what the agent interface accepts.

## Where samples come from

A panel binds through `scope::DataSource` and never learns whether the source
is the live bus or a recording. `bind()` turns a message into a `double`;
`bindRaw()` hands over the bytes for panels that are not about numbers, with a
`RawClassifier` the panel supplies so the source stores the answer
uninterpreted. When the window swaps sources, panels release their handles
against the old source before the pointer moves, because a handle means nothing
to a source that did not issue it. A panel that keeps a handle across
`setSource()` is the bug that rule exists to prevent.

{: .warning }
A plotted buffer's times must be non-decreasing. The binary search in
`SampleHistory::lowerBound()` assumes it and cannot detect otherwise; it returns
a plausible wrong index and everything downstream computes from the wrong
samples with nothing logged. Seeking backwards is what breaks it, which is why
a seek clears before it refills.

## Checking it

`scope_sample_stats` and `scope.stats` are the assertions to reach for before a
screenshot: a picture shows a line, the stats say what the line is made of. The
faster loop is a recording: `bag record` a few seconds, open it in scope, seek
to an exact instant and assert on the stats. Seeking backwards is the case worth
checking. The [scope](../apps/scope.html) page covers the recording workflow
and [scope internals](../design/scope-internals.html) the reasoning behind the
seams.
