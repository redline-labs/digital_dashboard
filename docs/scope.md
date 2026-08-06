# scope — the live time-series visualizer

The third GUI application. `dashboard` shows what a value *is*; `scope` shows
what it has been *doing*.

```bash
cmake --build build --target scope
./build/scope/scope                                    # empty, pick signals live
./build/scope/scope -c configs/scope/engine_demo.yaml   # a saved workspace
./build/scope/scope --mcp                               # headless, agent-driven
```

There is nothing to see without traffic on the bus. For a car-free bring-up:

```bash
./build/mock_data/test_data_publisher &
./build/scope/scope -c configs/scope/engine_demo.yaml
```

## The shape of it

```
                 TimeBase  ── one render timer for the whole window
                    │         one clock, one cursor, one pause
       ┌────────────┼────────────┐
       ▼            ▼            ▼
    Panel        Panel        Panel      QDockWidgets: dock, tab, split, float
       │            │            │
       └────────────┴────────────┘
                    │  SignalBuffer per plotted signal
                    ▼
               DataSource        ← the seam
                    │
            LiveZenohSource      (a RecordedSource would sit here too)
```

**DataSource is the seam that matters.** Live and recorded data are the same
shape — the same zenoh messages, the same capnp schemas, the same expressions
over their fields — and differ only in where the bytes and the timestamps come
from and whether you can seek. `caps()` reports that; the transport bar renders
from it; no panel ever learns which kind is behind it. Reading recorded data is
a new implementation of one interface, not a change to every panel.

**Panels are registered in one place.** `scope/include/scope/panel_table.h` is
the list; the enum, the config variant, the Panels menu, the YAML decoder and
the agent interface's idea of what exists all derive from it. Adding a video or
tabular panel is one line there plus a directory — the registration steps are
written out at the top of `scope/include/scope/panel_registry.h`.

**Picking a signal knows nothing about panel types.** The browser produces a
`BindingCandidate`, the drag carries it, the dialog returns it, and every panel
answers the same two questions: `acceptsBinding()` and `addBinding()`. A
time-series plot takes numeric fields and declines whole topics; a video panel
would do the reverse. Neither the browser nor the drag plumbing changes.

## Timestamps, and why they are what they are

Samples are stamped **on arrival**, by the source, with `steady_clock`. Neither
alternative works:

- **zenoh's own timestamps are off.** `SessionManager::buildConfig()` never sets
  `timestamping.enabled`, and `detail::SampleMeta` exposes only the encoding, so
  there is nothing to read even if it were on.
- **payload timestamps exist on five schemas.** `EngineRpm`,
  `EngineTemperature`, `VehicleSpeed`, `VehicleOdometer`, `VehicleWarnings` —
  the mock and legacy ones. Every real telemetry schema has none: `MotecM1`
  across its thirty-odd structs, `MotecPdm`, `Megasquirt`,
  `RacegradeTc8Signals`, `MotecLtc`. `CanFrame` has one in microseconds on a
  backend-defined epoch, with `0` meaning "no timestamp".

A payload-timestamp fast path would be right for five schemas and wrong for the
rest, and mixing two clocks on one axis is worse than one honest arrival clock.
Stamping is the *source's* job precisely so a recorded source can supply
recorded times instead.

One consequence worth knowing: every signal carried by the same message gets the
same stamp, read once per callback. That is what makes two fields of one topic
line up exactly under the shared cursor.

## Buffering

Two stages, and the split is the point.

| | written by | on overflow |
|---|---|---|
| `StagingRing` | the zenoh RX thread, lock-free, no allocation | drops the **newest**, counts it |
| `SampleHistory` | the GUI thread, on the render tick | overwrites the **oldest** |

Opposite directions on purpose. The staging ring cannot drop its oldest without
the producer moving an index the consumer owns. The history should keep its
newest, because a plot whose right edge stopped updating would look like a hang.

This is deliberately **not** `dashboard::ExpressionSubscription`, which is the
tree's usual way onto the GUI thread. That is a single-slot mailbox: it keeps
the newest sample between ticks and discards the rest, which is right for a
gauge and fatal for a plot, because the samples it throws away are the line.

`scope_sample_stats` reports `dropped`. Anything above zero means the trace is
lying about the data.

## Drawing

Traces are reduced to one min/max span per pixel column
(`scope/include/scope/decimate.h`), so a frame costs the width of the widget
rather than the size of the history. The vertical span per column is what keeps
spikes visible through decimation — "every Nth sample" deletes exactly the
extremes you opened a scope to see — and the first/last values are what join
columns so a smooth signal does not draw as a comb.

Two vertical axes. Engine rpm and oil pressure differ by three orders of
magnitude; on one scale the smaller is a flat line whose shape you cannot read.
`right_axis: true` on a trace scales it against the right instead, and the
legend marks it `[R]`.

`qt_helpers::CachedPaintWidget` is deliberately not used here. The layer it
caches would be the grid, and with autoscale on — the default — the vertical
range changes whenever the visible data does, so the cache would be invalid
almost every frame. The reasoning is in the panel header so nobody adds it back.

## Discovery is observation, not query

zenoh has no retained messages, so the only way to know a topic exists is to see
a sample of it. `pub_sub::observeTopics` subscribes for a window and reports what
arrived.

Everything downstream has to respect that. An empty browser means "nothing
published in the last N seconds", never "no topics" — and the UI says exactly
that. A rescan **merges** rather than replacing, or a topic published every ten
seconds would flicker in and out of the list depending on whether its sample
landed inside the window. `scope.browser` repeats the caveat in every reply.

If a signal seems missing, try a longer window before concluding anything.

## Workspaces

YAML in `configs/scope/`, built from `REFLECT_STRUCT`, so it is hand-editable
and diffable. A byte-stability test pins that saving twice does not keep
changing the file.

The dock arrangement is a base64 `QMainWindow::saveState()` blob stored
*alongside* the readable half, never instead of it. It is opaque and
Qt-versioned, so a failed restore is a warning and a default layout: everything
that matters is in the YAML, and losing the blob costs a re-drag, not data.

Two rules follow from `restoreState()` matching docks by `objectName` and
silently dropping any it cannot find:

- every dock gets an `objectName`, assigned by `addPanel()` and nowhere else;
- a panel with no `id:` is warned about at load, because its saved position
  cannot be matched back to it.

An unknown `type:` is a **warning**, not an error — a workspace written by a
newer build may name a panel this one has never heard of, and refusing the file
would make an upgrade one-way. The panel is skipped and said so.

## Driving it

Everything in `docs/agent_control.md` applies; scope registers `ui.*`,
`input.*`, `app.*` and `zenoh.*` like the others, plus:

| Method | Purpose |
|---|---|
| `scope.panels` | panels, their traces, and the types available |
| `scope.add_panel` / `scope.remove_panel` | compose the window |
| `scope.add_signal` / `scope.remove_signal` | bind and unbind |
| `scope.browser` | the topic→field tree, optionally rescanning first |
| `scope.browser_drag` | drive the drop path itself |
| `scope.time_base` | window length, pause, cursor, and the source's caps |
| `scope.panel_get_config` / `_set_config` / `_describe_config` | reflected config |
| `scope.save` / `scope.load` | workspaces |
| `scope.sample_stats` | what each signal actually received |

The loop that closes fastest:

```
app_launch(app="scope")
scope_add_panel(type="time_series", id="plot1")
scope_browser(rescan=true)
scope_add_signal(panel="plot1", zenoh_key="vehicle/engine/rpm", field="rpm")
scope_sample_stats()          # count, drops, min/max — is the data there at all?
ui_screenshot()               # and then actually look
```

**`scope.sample_stats` is the one to reach for first.** A screenshot shows a
line; this says what the line is made of. `received` climbing with `dropped` at
zero and a `min`/`max` bracketing what you published is a much stronger
statement than a picture, and it is the assertion a test can make.

`scope.browser_drag` exists for the same reason `editor.palette_drag` does:
`QDrag::exec()` runs a nested loop over real platform events that synthesized
ones cannot advance, and on the offscreen platform may not run at all. It sends
the DragEnter → DragMove → Drop triple straight to the target, covering the
accept/reject logic and the drop handler — everything worth testing — and
leaving only the few lines inside `exec()`.

## Tests

```bash
ctest --test-dir build -R scope --output-on-failure
```

- `scope_test_ring` (unit) — the lock-free hand-off, the retained history, and
  the retention limits, including a real two-thread run of 200k samples.
- `scope_test_decimate` (unit) — column reduction against a brute-force
  reference over random windows. Every way this can be wrong looks like data
  rather than like a bug.
- `scope_test_workspace` (unit) — the codec, weighted towards hand-edited files
  that are wrong in the usual ways.
- `scope_test_panels` (gui) — the window and panel layer against a real widget
  tree, using a stub `DataSource` so it needs no bus. This one caught a live
  crash: `{"zenoh_key": 42}` in dropped data threw a `json::type_error` out of a
  Qt drop handler, which terminates the app.
