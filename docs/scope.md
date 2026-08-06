# scope — the live time-series visualizer

The third GUI application. `dashboard` shows what a value *is*; `scope` shows
what it has been *doing*.

```bash
cmake --build build --target scope
./build/scope/scope                                    # empty, pick signals live
./build/scope/scope -c configs/scope/engine_demo.yaml   # a saved workspace
./build/scope/scope --bag drives/2026-08-06             # review a recording
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
       ┌────────────┴────────────┐
       ▼                         ▼
 LiveZenohSource           RecordedSource
                                 │
                        ┌────────┴────────┐
                        ▼                 ▼
                 BagFileProvider    CaptureProvider
                 (a bag on disk)    (the in-memory capture)
```

**DataSource is the seam that matters.** Live and recorded data are the same
shape — the same zenoh messages, the same capnp schemas, the same expressions
over their fields — and differ only in where the bytes and the timestamps come
from and whether you can seek. `caps()` reports that; the transport bar renders
from it; no panel ever learns which kind is behind it. Reading recorded data is
a new implementation of one interface, not a change to every panel — which is
what the tree claimed for a year before anything tested the claim, and it held.

**The source is reseatable.** `ScopeWindow::setSource()` swaps the whole thing:
into review over a recording, or back to live. The sequence is the substance —
panels rebind FIRST, while the old source is still alive and can honour their
releases; the browser and the time base follow; only then is the old source
destroyed. A handle means nothing to a source that did not issue it, so
repointing before releasing leaks every subscription on the old one.

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

- **zenoh's publish timestamp is the publisher's WALL clock.** It exists now --
  `SessionManager::buildConfig()` enables `timestamping` and `detail::SampleMeta`
  exposes it -- and it is still the wrong clock for a scrolling plot. It steps
  when NTP disciplines it, it is plainly wrong on a unit that booted with a dead
  RTC, and zenoh re-stamps rather than rejects a time too far in the future, so
  the error arrives looking ordinary. An axis that jumps backwards mid-trace is
  worse than one measured from the wrong origin. See `pub_sub/timestamp.h`.
- **payload timestamps exist on five schemas.** `EngineRpm`,
  `EngineTemperature`, `VehicleSpeed`, `VehicleOdometer`, `VehicleWarnings` —
  the mock and legacy ones. Every real telemetry schema has none: `MotecM1`
  across its thirty-odd structs, `MotecPdm`, `Megasquirt`,
  `RacegradeTc8Signals`, `MotecLtc`. `CanFrame` has one in microseconds on a
  backend-defined epoch, with `0` meaning "no timestamp".

A payload-timestamp fast path would be right for five schemas and wrong for the
rest, and mixing two clocks on one axis is worse than one honest arrival clock.
Stamping is the *source's* job precisely so a recorded source can supply
recorded times instead -- and a recorded source is where the publish timestamp
earns its keep, because a bag stores both and can say which one it is showing.
See `docs/bag.md`.

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

**Seeking bypasses both stages.** `SignalBuffer::replaceHistory()` clears and
refills the history directly, because a seek loads a whole retention window —
five minutes of a 1 kHz signal is 300k samples — and the staging ring holds
4096. Pushed through staging, all but the last 4096 would be counted as drops.

The clear is not optional either. `SampleHistory::lowerBound()` is a binary
search that **assumes non-decreasing time** and has no way to notice when that
is false: it returns a plausible wrong index, and the autoscale, the decimation
and the cursor readout all quietly compute from the wrong samples. Scrubbing
backwards is the one operation that can break it, so the buffer never holds
samples from two scrub positions at once. `scope_test_recorded_source` pins
both halves of that.

## Capture, and reviewing it

Scope records **the whole bus** into memory from the moment it starts — `**`,
no exclusions. That is the point: a signal nobody thought to plot can still be
added afterwards, and a filter taken from the panels would only ever capture
what was already on screen.

The capture is bounded by **bytes and time, whichever binds first**, evicting
oldest. Both bounds are needed and for different workloads:

| workload | rate | which bound bites |
|---|---|---|
| CarPlay streaming (H.264 + raw PCM) | ~1.5 GB/hour | **bytes** — a time-only cap is OOM-killed in minutes |
| telemetry alone | ~11 MB/hour | **time** — a byte-only cap never bites at all |

Defaults are 1 GiB / 30 minutes, both in the workspace
(`max_capture_bytes`, `max_capture_seconds`; `0` disables either).

Eviction is **counted and shown**, next to the retained span in the transport
bar. A capture silently dropping its head is the same class of lie as a
recorder dropping samples: the start of a trace reads as a publisher that had
not started yet. `bag record` already treats it that way, and a saved capture
records the eviction count as the bag's `dropped_messages`, because from the
file's point of view that is exactly what it is.

**Capture keeps running while you review it.** Deciding to look at something
must not cost you everything that happens while you look. The consequence is
that the provider's span moves under the scrubber — so the transport bar
re-reads the range every frame, and the retained span is on screen.

Saving goes through `bag::BagWriter`, so the result is an ordinary bag: `bag
info`, `bag verify`, `bag play` and Foxglove all work on it, with the schema
descriptors the writer already knows how to embed.

## Reviewing a recording

**Decode once per signal, not once per scrub tick.** This is the load-bearing
decision. `bind()` starts one pass over the whole recording on a background
thread, decoding that signal into a flat `std::vector<Sample>`; seeking is then
a slice out of it. Four hours of a 25 Hz signal is 360k samples, under 6 MB.

The alternative is not merely slower. `BagReader::forEach` constructs and opens
an `mcap::McapReader` **per part, per call**, and on a part with no summary it
falls back to scanning the entire data section — driven from a slider that is
re-opening files thirty times a second and re-scanning a torn recording every
one of them.

The pass is asynchronous, so `bind()` returns a usable handle before any data
exists. A trace is empty until its decode finishes, which is the same state a
live signal is in before its publisher says anything. `scope.source` reports
`decodes_pending`; anything reading `sample_stats` or screenshotting before
that is zero is looking at an unfinished picture, not a broken one.

**Time is seconds since the recording started**, matching the live source's
"seconds since construction" shape, so `viewEnd()`, `viewBegin()` and the
panels' relative axis labels work unchanged. The wall clock a recording
genuinely has — and the live source does not — appears in the cursor readout.

`log_time` is what is plotted, not `publish_time`: the recorder's clock is
monotone, the publisher's is a wall clock that can step backwards under NTP or
be plainly wrong on a unit that booted with a dead RTC. That would not just
mislabel the axis, it would violate `lowerBound()`'s precondition outright.

A message recorded under a different schema than the binding expects is
**skipped, not decoded**. capnp reads whatever bytes it is handed against
whatever schema it is given — field offsets simply land elsewhere — so the
result would be a plausible wrong number rather than an error.
`ExpressionEvaluator::checkPublishedSchema()` is deliberately not used here: it
takes a full encoding string (`application/capnp;EngineRpm`) and
`BagMessage::schema` is the bare registry name, so passing it through would
silently check nothing.

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

## Discovery

Topics appear in the picker **as soon as a node starts**, whether or not it has
ever published. There is no rescan, no polling for traffic, and no window to
wait for.

Every publisher declares a zenoh **liveliness token** when it is constructed
(`detail::BytePublisher`). A token carries no payload -- the key expression is
the whole message -- and nothing is ever sent on it, so this adds no periodic
traffic. It is a key that exists while its process does.
`pub_sub::TopicDirectory` watches that space with `history = true`, which
replays tokens declared before it subscribed, so a browser opened after the
nodes still sees all of them.

It goes in `BytePublisher` rather than in each node because the key and the
schema are already both present at that one constructor. Every publisher in the
tree therefore advertises with no node changes, and the advertisement cannot
drift from the `application/capnp;<Schema>` encoding stamped on each sample --
both derive from the same two arguments. The per-sample encoding stays
authoritative; this is not a second source of truth.

**The browser asks the source, not the bus.** `DataSource::topics()` is cheap
and non-blocking, and the browser polls it on a timer, comparing
`topicsRevision()` first so an idle tick costs one atomic load. "Which topics
exist" is a property of where the data comes from, so a recorded source will
answer the same question from its file index with nothing above it changing.
That is also why the method takes no window: an interface saying "listen for N
milliseconds" would describe one implementation's mechanism rather than the
question being asked.

A topic whose publisher goes away is **greyed, never evicted**. A row the user
may have bound must not vanish underneath them, and a liveliness DELETE means
*unreachable from here* rather than *gone* -- a network partition and a crash
are indistinguishable.

> **What this deliberately does not tell you:** that data is flowing. A token is
> up while its process is, so a CAN bridge with an unplugged adapter still
> advertises its topics. That is the right answer for a picker -- the topic
> exists and is bindable -- and `scope_sample_stats` is where you find out
> whether anything is actually arriving.
>
> `pub_sub::observeTopics` still exists for the tools whose job *is* to report
> traffic (`nodes/inspect list`, `zenoh_list`). Scope no longer uses it: it
> blocks for its whole window, which froze the UI on every press of the button
> it served.

## Topic key rules

The advertisement key is `@redline/adv/<schema>/<mangled topic>/<zid>`, and both of
those choices are load-bearing.

The **leading `@`** is what keeps advertisements out of the `**` subscriptions
that topic observation uses. Zenoh treats a segment beginning with `@` as
*verbatim*: no wildcard, not even `**`, will match it. Without it every `**`
subscriber in the tree would receive our own advertisements and report them as
topics. Zenoh's admin space (`@/...`) and ROS 2's liveliness space
(`@ros2_lv/...`) rely on the same property.

The **mangling** replaces `/` with `%`, following ROS 2's `rmw_zenoh`, so a topic
name occupies exactly one key segment. Without it, a name spread across several
segments and neither field could be wildcarded.

That only works while `%` cannot appear in a topic name, so the charset is
**enforced** rather than assumed -- `[A-Za-z0-9_-/]`, no leading or trailing `/`,
no empty segments. Four characters are called out individually because each
fails *quietly*:

| character | what would happen |
|---|---|
| `%` | demangles into a different topic name; the picker offers a signal that can never bind |
| `@` | the topic becomes invisible to every wildcard subscription, including discovery |
| `* $ ? #` | zenoh rejects the key, so the publisher never declares and silently sends nothing |

The rule is checked at all three points a key enters the system:

- the **editor's properties panel** refuses it as you type -- field turns red,
  the reason appears under the form, Apply is disabled;
- **config load** (dashboard and scope) reports it as an error with the field
  path, e.g. `widgets[0].config.zenoh_key`, and refuses to load;
- **`BytePublisher`** refuses to come up at all.

Subscribers get a weaker rule, since they may wildcard and discovery itself
subscribes to `**`.

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
| `scope.time_base` | window length, pause, cursor, caps — plus `seek` / `playing` / `rate` |
| `scope.panel_get_config` / `_set_config` / `_describe_config` | reflected config |
| `scope.save` / `scope.load` | workspaces |
| `scope.source` | which kind of source is behind the panels, and `decodes_pending` |
| `scope.open_recording` / `scope.go_live` | review a bag directory, or return to the bus |
| `scope.capture` | messages, bytes, retained span, **evicted** |
| `scope.review_capture` / `scope.save_recording` | review the capture; write it out as a bag |
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

And the one that proves scrubbing works, which a screenshot cannot:

```
app_call("scope.open_recording", {"path": "/tmp/demo"})
app_call("scope.time_base", {})        # caps.live=false, seekable=true, t_begin/t_end
app_call("scope.add_signal", {...})
app_call("scope.time_base", {"seek": 5.0})
scope_sample_stats(...)                # t_first/t_last bracket the sought window
app_call("scope.time_base", {"seek": 1.0})   # BACKWARDS
scope_sample_stats(...)                # retained SHRINKS, t_last moves back with it
```

The backwards seek is the one to check. A buffer that kept the position it came
from stays perfectly ordered and is completely wrong, so "still ordered" is not
the assertion — `t_last` moving back is.

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
- `scope_test_recorded_source` (unit) — scrubbing, over a stub provider with
  synthetic messages so "seek to 5 s and you get exactly the samples in
  `[5 - history, 5]`" is an exact assertion. The load-bearing case is the
  **backwards** seek, and it is checked two ways because the two failures look
  different: without the clear, times go backwards mid-buffer; without the
  rebuild, the window is stale and perfectly ordered.
- `scope_test_capture_buffer` (unit) — eviction by bytes, by time, and the case
  a buffer honouring only one bound would get wrong on the other workload. Plus
  the accounting invariant: every pushed message is either retained or counted
  as evicted, checked under real threads.
- `scope_test_workspace` (unit) — the codec, weighted towards hand-edited files
  that are wrong in the usual ways.
- `scope_test_panels` (gui) — the window and panel layer against a real widget
  tree, using a stub `DataSource` so it needs no bus. This one caught a live
  crash: `{"zenoh_key": 42}` in dropped data threw a `json::type_error` out of a
  Qt drop handler, which terminates the app. It also pins the source-swap
  ordering rule, and that `history_seconds` reaches the panels — it round-tripped
  through the YAML perfectly for a year while both ends ignored it.

## Headless

`--mcp` sets `setHeadless(true)`, and that is not only about the Qt platform.
There is nobody to dismiss a modal dialog in a headless run, so one raised there
does not fail — it **hangs the process, with no log line and no error**, which
is the hardest kind of bug to find from the other side of a socket.

So the File menu's work is split three ways, copied from the editor:
dialog-free `loadWorkspace()` / `saveWorkspace()` / `openRecording()` /
`saveCaptureTo()` that everything except a menu item calls; thin dialog
wrappers that only pick a path; and `confirmDiscardChanges()`, which
early-returns `true` with a warning under `headless_`.

Quitting prompts about an unsaved **capture** before an unsaved workspace. A
workspace can be rebuilt by hand in a couple of minutes; a capture of what the
vehicle was doing cannot be rebuilt at all.
