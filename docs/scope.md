# scope — the live time-series visualizer

The third GUI application. `dashboard` shows what a value *is*; `scope` shows
what it has been *doing*.

```bash
cmake --build build --target scope
./build/scope/scope                                     # OFFLINE and empty — the default
./build/scope/scope --bag drives/2026-08-06             # offline, over a recording
./build/scope/scope --online                            # attach to the bus and capture
./build/scope/scope -c configs/scope/engine_demo.yaml   # a saved workspace
./build/scope/scope --mcp                               # headless, agent-driven
```

**Scope starts OFFLINE and does not open a zenoh session until you ask it to.**
Load a bag to scrub through, or press `Offline` on the toolbar to go online and
watch the live bus. `--online` is the startup form; it is mutually exclusive
with `--bag`, because a bag is an offline source and asking for both asks for
two different things.

There is nothing to see online without traffic on the bus. For a car-free
bring-up:

```bash
./build/mock_data/test_data_publisher &
./build/scope/scope -c configs/scope/engine_demo.yaml --online
```

## The shape of it

```
                 TimeBase  ── one render timer for the whole window
                    │         one clock, one cursor, ONE VIEW WINDOW
       ┌────────────┼────────────┬──────────────┬──────────────┐
       ▼            ▼            ▼              ▼              ▼
  TimeSeries      Table        Video      TimeSeries    OverviewStrip
     Panel        Panel        Panel         Panel      (the whole recording,
       │            │            │             │         and the window on it)
       └────────────┴────────────┴─────────────┘
                    │  SignalBuffer per plotted signal
                    │  RawBuffer    per bound stream
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
from and whether you can seek. `caps()` reports that; the two bars render
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
the list; the enum, the config variant, **the stats variant**, the Panels menu,
**the toolbar's Add buttons**, the YAML decoder and the agent interface's idea of
what exists all derive from it. Adding an XY panel is one line there plus a
directory — the registration steps are written out at the top of
`scope/include/scope/panel_registry.h`. The `video` panel was the first to go
through that recipe end to end, and adding it needed **no change to
`scope_methods.cpp` at all**. The `table` panel was the second, and needed one:
not for anything panel-specific, but to stop `scope.remove_signal` casting to
`TimeSeriesPanel` — see below.

## The two bars

**The top bar is about mode and composition; the bottom bar is about time.**

```
[Offline ▾][Load Recording…]  drives/2026-08-10 · 412 s   ∿ Time Series   − + ⤢ Fit   Open Save   Signals
[● Online ▾][Load Recording…]  ⏺ capturing · 41k messages  ∿ Time Series   − + ⤢ Fit   Open Save   Signals
```

**One button for one bit of state, and it reads the state it is IN.** A window
is online exactly when its source tails the bus; there is nothing else to store.
`Offline` / `● Online` rather than a Go Online / Go Offline label, because a
button named for its action has to be read together with its checked state to
know which way round it is, and half the people reading it will get that wrong.

It replaced a checkable pair (`mode_live` / `mode_review`) — a hand-rolled radio
group for a boolean, whose two halves were free to disagree.

The dropdown holds Load Recording…, Review Session Capture and Save Recording…,
and `Load Recording…` is also promoted onto the bar in its own right: with
offline as the default, opening a bag is the primary action of a freshly started
window, and it used to be two clicks deep behind a button labelled "Review" — a
word that does not say "bag" to anyone looking for one.

The toggle is checked from **the source**, never from the click that changed it,
so a swap made by `--bag` at startup, by the agent interface, by an open that
failed or by a transition the user cancelled leaves the toolbar saying what is
actually behind the panels.

The chip beside it says **what is behind the panels** — the bag and its duration,
the session capture, or `nothing loaded`. It is not the capture's state: that is
`transport_status` at the other end of the window, and this label used to render
the same string from the same line of code. One string rendered twice in one
window is a tell that one of the two has no job of its own.

Everything on it is the **same `QAction`** the menus already own, not a copy.
One action means one objectName, one handler and one enabled-state, so the
toolbar cannot drift out of step with the menu item that does the same thing —
which is exactly what two copies would do the first time one of them grew a
guard. `scope_test_panels` pins that there is exactly one of each.

**Glyphs, not icons.** There is no icon pipeline in this tree — no `QIcon`
anywhere, and the one `.qrc` is dashboard widget artwork that scope does not
link. A panel's button glyph is a `kToolbarGlyph` on the panel class, harvested
into `PanelTypeInfo` exactly as `kFriendlyName` is. Swapping glyphs for `QIcon`s
later is a change to that field and to the toolbar, and to nothing else. Pick
one that is in the default font on every platform: a missing glyph renders as a
blank box, which reads as a broken button rather than a plain one.

**Configuring a panel is right-click ▸ Configure…** — one reflection-driven
dialog for every panel kind, built from the same `REFLECT_STRUCT` metadata
`scope.panel_describe_config` serves, so the next panel type gets a form with
no UI code. It applies through the same clamped path `scope.panel_set_config`
takes, so editing a colour or an axis never discards a trace's history, and an
out-of-range value shows its clamped self after Apply. The map panel's
`tileset` renders as a combo of the names configured in Settings — which is
what closed the old dead end where a map added from the GUI could never be
pointed at an archive.

**The dock's X removes the panel** — the same thing the context menu's Close
panel does. Qt's default merely HID the widget: still bound, still draining at
the render rate, saved hidden into `dock_state`, with no View-menu entry to
bring it back.

**The second panel splits below the first; the third tabs.** Two tabs can never
show the shared cursor, which is the whole point of a second panel — and a
hidden tab does not paint, so its stats read zero and it looks broken.

**Picking a signal knows nothing about panel types.** The browser produces a
`BindingCandidate`, the drag carries it, the dialog returns it, and every panel
answers the same two questions: `acceptsBinding()` and `addBinding()`. A
time-series plot takes numeric fields and declines whole topics; the video panel
does exactly the reverse. Neither the browser nor the drag plumbing changed when
the second panel type arrived — which is what the seam was for.

**Reading a panel knows nothing about panel types either.** `panelConfigOf()`,
`applyPanelConfig()` and `panelStatsOf()` are generated from the same table
(`scope/include/scope/panel_registry.h`), so the workspace codec and the four
`scope.*` methods that serve config and stats never name a panel kind. They are
free functions rather than virtuals on `Panel` because the variants are built
*from* the panel classes and `Panel` sits below them — a virtual returning one
would need a type `Panel` cannot see.

> **This is the seam that used to be missing, and it lost data.**
> `ScopeWindow::toWorkspace()` cast to `TimeSeriesPanel` with no `else`, so any
> other panel type saved its `type:` with the config left on `std::monostate` —
> which the YAML encoder omits entirely, so the panel came back
> default-constructed with nothing logged. `scope_test_panels` pins it now.

## States, and why they are not lines

An enum's ordinals are **labels, not quantities**. The gap between `iap2` (3) and
`ncmUp` (4) is not one unit of anything, so a line sloping between them draws a
transition that did not happen — the value was one, then it was the other. A bool
is worse: as a line it reads as a signal spending half its time at 0.5.

So an enum or a bool trace is drawn as a **lane** — a band per state, full width,
with the state's *name* written in it and zero-order-hold transitions. That is
what a logic analyser does and what every motorsport tool calls a digital
channel, and it composes with numeric traces instead of competing for the value
axis: gear and a PDM trip state sit under an rpm trace without either needing a
scale.

```
   rpm  ────╱╲───╱───────╲──────
        ┌──────────────┬────────────────┐
  phase │  airplayHandshake  │ recording │
        ├──────────────┴────────────────┤
  micActive │  false  │  true  │  false  │
        └───────────────────────────────┘
        -30       -20       -10        0
```

`display` on a trace is `automatic` (lane for an enum or a bool, line otherwise),
`line`, or `lane`. The override matters both ways: plotting gear against rpm on
the value axis is legitimate, and so is forcing a lane onto a small integer that
is really a state but which nothing in the schema marks as one.

**A lane is not on the value axis and must not stretch it.** An enum whose
ordinals run 0..7 sharing an autoscale with rpm flattens the rpm trace against
the top of the plot — so getting this wrong ruins the trace *beside* it, not just
the state channel. `scope.stats` reports `lane` per trace so which way a trace
went is checkable without a screenshot.

Names come from the **schema**, not from the drag: a workspace loaded from disk
never saw a `BindingCandidate`, and a trace that got its labels only when dragged
would lose them on the next reload. Only a bare field (`phase`) or one list
element (`values[7]`) resolves — `phase * 2` has left the enum's domain and is a
number, so it stays a line.

That resolution lives in `scope/include/scope/state_names.h` and is shared with
the table panel's cells, so a lane and a cell can never disagree about what a
state is called. A lane only has room for the name when the band is wide enough;
if you want the word every time, that is what [the table panel](#the-table-panel)
is for.

## Lists, and how many elements they have

The PDM keeps everything in lists: 32 output currents, 32 loads, 32 voltages, 32
statuses, 23 input states. `values[7]` is an ordinary expression, so each of
those is a plottable channel — but a capnp list **declares no length**. The count
is a property of each message, so a picker has nothing to expand.

Both ways round that are wrong. Guessing a count offers rows that do not exist,
and binding one produces no reading — on screen a flat empty trace,
indistinguishable from a dead publisher. Peeking at a live message makes the
browser show different things depending on whether traffic happened to be
flowing.

So the schema says it, with an annotation:

```capnp
using Annotations = import "annotations.capnp";

struct MotecPdmOutputCurrent {
  values @0 : List(Float32) $Annotations.fixedLength(32);
}
```

Annotations change **nothing** about the wire format — a message encoded before
one was added decodes identically after — and they travel inside the schema
node, so `pub_sub::fixedListLength()` reads them back at runtime with no side
table to keep in sync. `describeSchema` reports it as `fixed_length`, and the
browser expands the list into one draggable row per element, collapsed by
default. Dragging `values[7]` binds `values[7]`, with no hand editing.

**A list without the annotation is not plottable element by element**, and the
browser says so rather than offering a row that binds and then fails. That is a
deliberate limit. The alternative was discovering the length from the first
message and recompiling whenever it changed — which works, and costs a recompile
*per message* for a list that genuinely varies. `CanFrame.data` is `List(UInt8)`
of 1–64 bytes on the highest-rate topic in the tree, so that cost lands exactly
where it can least be afforded.

With the length settled at construction the expression compiles **once**, exprtk
range-checks every literal index against the real count, and `sum()`/`avg()`
divide by the right number by construction. A message that contradicts the
annotation is skipped and reported once — the declared length is a claim about
the schema, not a guarantee about every publisher.

## Navigating time

**One window, shared by every panel.** `TimeBase` owns `[viewBegin, viewEnd]`,
and every gesture in every panel moves *that* rather than anything of its own.
That is what makes the shared cursor mean something: two panels always show the
same span, so a value read off one lines up with a value read off the other.

Stored as **(right edge, span)**, not as a pair, for three reasons. The span is
what the workspace persists, what the spin box edits and what a panel with
`follow_time_base: false` overrides. It gives `setWindowSeconds()` a defined
answer to "which edge moves?" — the right one holds. And **a following right
edge has to be derived**: stored, it is only correct until the source's clock
advances, so anything reading it between two render ticks gets a window up to a
frame stale, silently.

| gesture | does |
|---|---|
| wheel | zoom about the pointer — the instant under it keeps its place |
| shift + wheel | zoom the value axis; turns autoscale off (per panel) |
| drag | pan |
| shift + drag | rubber-band a range, zoom to it |
| double-click | fit all, and give autoscale back |
| `Ctrl` `+` / `Ctrl` `-` / `Ctrl` `0` | zoom in / out / fit, anywhere in the window |
| `←` `→` `space` | pan back / pan forward / follow-or-play — except while the signal tree has focus, where they navigate the tree |

All six live in the **View menu**, which is where their shortcuts are
discoverable; the menu entries and the shortcuts are the same `QAction`s.

`following` is the one flag underneath `Mode{Live, Paused}`: it means *the right
edge is being driven by something other than the user* — the source's clock on a
live source, playback on a recording. Every pan and zoom clears it, because you
cannot hold a span still while it is pinned to now.

With one exception, and it exists because the alternative is a bug that reads as
a hang: **a gesture that lands the right edge exactly at `now()` on a live
source re-arms following.** Panning further right than there is data would
otherwise pin the view at the live edge with following off, and the plot simply
stops scrolling — which is indistinguishable from a dead publisher.

**Zoom-out stops at what is retained.** `availableRange()` is the recording's
extent on a seekable source, and `[now - history_seconds, now]` on a live one,
floored at zero because a live source's clock starts when it does. The window
*slides* into range rather than being squashed, so a pan near the edge never
silently changes the zoom level.

### The view's right edge IS the playhead

On a recorded source, and it is a rule rather than a coincidence.
`RecordedSource` loads `[t - history, t]` around **one** position on every seek
(`SignalBuffer::replaceHistory()`), so a view sitting anywhere other than the
playhead would be drawn from buffers holding a different stretch of the
recording. It would look like data, not like a bug. `setView()` therefore seeks
to its right edge, and `seek()` moves the whole window so its right edge lands
where it was told.

**Seeks coalesce to the render tick, unconditionally.** A gesture — drag OR
wheel — emits an event per pass of the event loop, 60 to 125 a second, and each
seek refills a whole retention window per bound signal. Eight traces of a 1 kHz
signal over the default 300 s retention is 2.4 million samples rebuilt per
event. Every view change therefore only PARKS a pending seek; the render tick's
`flushSeek()` applies the latest one before the frame is drawn, so however many
events landed in the last 33 ms, the buffers refill once. (This replaced a
drag-only `setInteracting()` bracket, which left wheel zoom paying the
per-event refill.)

The agent interface still sees settled buffers: the method dispatcher calls
`flushSeek()` before EVERY `scope.*` method, so "set the view, then read
`sample_stats`" observes exactly the buffers it asked for, with no per-method
list to keep in sync.

## The overview strip

The bottom bar is the whole recording at a glance, with the view window drawn on
it as a region you can grab. It replaced a `QSlider`, and the reason is worth
stating: a slider positions one value in a range whose **contents you cannot
see**, so finding an event in half an hour of capture meant dragging blind and
watching the plot.

Back to front it draws: the message-density histogram, the stretch the panels'
buffers can actually reach, the eviction head, the view window with a grab
handle on each edge, the playhead, and the shared cursor — the same cursor, on
the same clock, that the panels draw.

Drag the body to pan, drag an edge to zoom, click outside to jump, wheel to zoom
about the pointer. The strip itself touches nothing: it emits `viewRequested`
and the window decides what that means, so there is exactly one place where a
view change turns into a seek.

**The histogram is not free and is not drawn per frame.**
`CaptureBuffer::density()` walks the retained deque under the same mutex the
zenoh RX thread needs to push, so recomputing it at the render rate would stall
the *producer*, not merely cost the consumer. `CaptureBuffer::revision()` is
useless as a cache key here — it bumps on every push, thousands a second — so
the 500 ms throttle is what bounds the work, and a resize or a source swap
forces a recompute immediately because the cached counts then describe a range
that is no longer on screen.

Each backend answers differently, and each says which it is:

| source | answers from | exact? |
|---|---|---|
| the in-memory capture | every retained message | yes |
| a bag on disk | `metadata.yaml`'s **part index**, opening no file | no |
| the live bus | the recorder's capture, via the epoch pair below | yes |

A bag's answer is approximate because "how many" is indexed and "where inside a
part" is not, so a single-part recording draws as one flat block. Counting
through `forEach` instead is exactly what that method's warning forbids — it
opens an `mcap::McapReader` per part per call and can fall back to scanning the
whole data section — and it would be driven from a widget.

A live source keeps no history of its own, so `DataSource::density()` declines
and `ScopeWindow` asks the recorder instead. That needs the two clocks
reconciled: the capture stamps UNIX nanoseconds and `LiveZenohSource` counts
seconds from its own construction, so the source samples the wall clock **once**
beside its steady epoch (`epochWallNanos()`). Once, because re-reading it would
slide the strip's background under a plot whose samples never moved, the first
time NTP disciplined the machine.

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

While it is **online**, scope records **the whole bus** into memory — `**`, no
exclusions. That is the point: a signal nobody thought to plot can still be
added afterwards, and a filter taken from the panels would only ever capture
what was already on screen.

Offline it records nothing, and opens no zenoh session at all. A window that
joined the bus before anyone asked would make "Offline" a label rather than a
fact — and scope is a diagnostic tool, so an instance that quietly attaches to
the system you are measuring is exactly what you do not want running unattended.

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

**The capture is a snapshot of the online session, not a tail.** It starts when
you go online and stops when you go offline, so reviewing it does not grow it.

This is the cost side of making "Offline" mean something. The capture used to
keep running while you reviewed it — deciding to look at something did not cost
you what happened while you looked — but that only worked because scope was on
the bus from the moment it launched, whatever the toolbar said. With offline as
the default and the honest meaning attached to it, **the interval you spend
scrubbing is not captured**, and a rare event you went back online to catch can
be missed. Go back online before you need the next one.

`ScopeRecorder::stop()` drops **the subscriber only** and keeps the
`CaptureBuffer`, which is the part that has to stay that way: going offline hands
the panels a `RecordedSource` over a `CaptureProvider` holding a pointer into
that buffer. Destroying the recorder instead would leave it reading freed memory
on the next render tick — a use-after-free that draws plausible data rather than
crashing. The buffer is dropped only when a NEW online session starts, and only
after an unsaved-capture prompt.

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
(`scope/include/scope/decimate.h`): the samples inside the window are walked
once (a binary search finds the left edge, so retained history outside the
window costs nothing) and the DRAW CALLS are bounded by the width of the
widget. The autoscale is derived from the same columns, so one pass serves
both. The vertical span per column is what keeps
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

## The video panel

The second panel type, and the one the seams above were designed for. It renders
the CarPlay H.264 stream at the shared time base's position, so a value read off
a plot under the shared cursor and the picture beside it are the same instant.

```bash
./build/nodes/carplay/carplay --config configs/carplay/carplay.yaml --simulate &
./build/scope/scope
# Panels ▸ Add ▸ Video, then drag the nodes/carplay/video TOPIC row onto it
```

It takes the **topic**, not a field of it. `acceptsBinding()` is the mirror of
the plot's: topic-level only, schema `CarPlayVideo` (capital P — `panel.h`
predicted this panel years early and misspelled it), and one stream per panel.

### Raw payloads needed a new seam

`DataSource::bind()` delivers `Sample{t, v}`. An H.264 access unit cannot travel
through that, and turning one into a double is not a lossy answer but a
meaningless one. `bindRaw()` hands over the bytes exactly as they arrived.

It is **schema-agnostic on purpose**: the moment that interface knows what a
`CarPlayVideo` is, one panel's schema is in the interface every panel shares. But
a recorded source cannot seek a stream with dependencies between messages unless
it knows where the independent ones are. So the consumer supplies a
`RawClassifier`, the source calls it and stores the opaque result, and two bits
of `RawMessage::flags` are reserved:

| flag | means | video |
|---|---|---|
| `kSeekPoint` | a message a consumer can start from | a keyframe |
| `kPreamble` | state that must be replayed before the seek point after it | SPS/PPS |

A stream that sets `kSeekPoint` on everything is seekable everywhere; one that
sets it on nothing is simply not seekable, which is the honest answer rather than
a broken picture.

### The recorded path is NOT "decode once per signal"

That strategy is right for a sixteen-byte sample and wrong by three orders of
magnitude for video: half an hour of CarPlay at 4 Mbit is ~900 MB of payload
against the ~6 MB four hours of a 25 Hz signal costs. Two stages instead:

1. **An index pass** at `bindRaw()`, holding `{t, size, flags}` and **no
   payloads** — about 1.3 MB for that same half hour. It is what the scrubber
   draws its ticks from and what a seek binary-searches. Counts toward
   `decodes_pending`.
2. **One seek-point-to-seek-point window** of payloads around the position,
   loaded on the worker thread. At a two-second GOP that is ~60 access units,
   about a megabyte. **Scrubbing inside the loaded window reads nothing at all.**

> **The window starts at the preamble, not at the keyframe.** Parameter sets are
> published just ahead of the keyframe they describe, and a window starting one
> message later decodes to nothing until the *next* keyframe. The first version
> marked config as both `kSeekPoint` and `kPreamble`, which made the config its
> own one-message GOP: the panel held 128 bytes, decoded zero frames, and kept
> displaying the picture from before the seek — a stale frame, which looks
> exactly like a seek that worked.

### Decoding is forward-only, which is the shape of the whole panel

There is no such thing as decoding the frame at *t*; there is only decoding from
the last seek point up to *t*. So a seek re-runs the decoder over a GOP, and
doing that synchronously would block the event loop for as long as the GOP is —
hence a **per-tick decode budget** of ~10 ms, spreading a catch-up over two or
three frames rather than stalling the window.

`scope/panels/video/video_decoder.{h,cpp}` is a **second** decoder, not a refactor
of `CarPlayWidget`'s. That one is load-bearing on a screen someone is touching and
its settings are measured against a real capture; pulling it out from under a
working CarPlay path to serve a review tool is a bad trade. What was copied is the
*knowledge* — the four subtleties are listed at the top of the header — plus one
thing a live stream never needs: `reset()`.

Two decisions there are worth knowing:

- **`thread_count` stays at 1.** Frame threading withholds `thread_count - 1`
  pictures before emitting the first. On a live stream that is latency; on a seek
  the last frame of the catch-up is exactly the one being asked for, and it would
  never come out.
- **The source-clock time rides through libavcodec on the packet's `pts`.**
  Stamping whatever comes out with the time of the last access unit submitted is
  correct only while decode order is display order — true for CarPlay today, but
  a fact about the phone's encoder rather than about this code. `frame_t` is the
  field a seek asserts on, so a wrong one turns the check that could catch a bad
  seek into one that agrees with it.

### The panel's own scrubber

A short strip along the bottom, so the panel is useful by itself — floated on a
second monitor, or as the only panel in a workspace, without the main window's
bottom bar in view. It is a dumb painter like `OverviewStrip`: it knows nothing
about `TimeBase`, emits `seekRequested`/`interactionChanged`/`cursorRequested`,
and the panel wires those to the shared clock.

It drives the **shared** time base rather than a position of its own. On a
recorded source the view's right edge *is* the playhead, so a video panel
scrubbing independently would draw a frame from one instant beside traces from
another — and it would look like data rather than like a bug. Tick marks show the
seek points, which are the instants a scrub lands on exactly.

Hidden when the source is not seekable. A seek bar that looks draggable and does
nothing reads as a broken panel.

## The table panel

A readout of what every bound signal is doing **right now**, one row each.

```bash
./build/scope/scope
# Panels ▸ Add ▸ Table, then drag fields onto it exactly as you would onto a plot
```

```
  Signal          │        Value │    Age
  ────────────────┼──────────────┼───────
  phase           │ airplayHands…│  12 ms
  micActive       │         true │  12 ms
  rpm             │         6120 │   4 ms
  oilPressurePsi  │           45 │  3.1 m   ← stale, and coloured for it
                  ↕              ↕
                  drag either divider to resize the column right of it
```

It is the complement of a plot rather than a lesser version of one. A plot
answers *what has this been doing*; forty signals of that is forty traces nobody
can read. This answers *what is everything at, this instant* — the question a pit
wall asks and the one a plot is worst at.

**It takes exactly what a plot takes**, which is the opposite arrangement from
the video panel's mirror-image `acceptsBinding()`. Two panels accepting the same
candidate is not a conflict: a drop goes to the panel it landed on, and a browser
double-click goes to the first panel that will have it, which was already the
rule. So the same drag fills a plot and a table, and the fields a plot reads
*worst* are the ones a table reads best.

### An enum reads as its name

A plot draws a state lane and writes the name in the band **when the band is wide
enough to hold it**; at a tight zoom, or on a state that changes quickly, it is a
colour and nothing else. A cell always has room for the word. `3` and `iap2` are
the same number and only one of them is an answer.

The names come from the same resolver the lanes use —
`scope/include/scope/state_names.h`, hoisted out of the plot when the table
needed it — so the two cannot disagree about which fields have names, what those
names are, or that `phase * 2` has left the enum's domain and is a number.

`format` per row is `automatic` (state names for an enum or a bool, a number
otherwise), `number`, `hex`, or `state`. The overrides matter both ways, as they
do for a lane: reading an ordinal against a raw CAN trace is legitimate, and so
is naming a gear number that nothing in the schema marks as a state. `hex` is
for the status words where the decimal form tells you nothing about which bits
are set.

### And it says how old the reading is

**This is the one failure mode a table has and a plot does not.** A plot shows the
line stopping, which is unmissable. A table shows a dead publisher's last value
in exactly the typeface it shows a live one — so the age column and the stale
colouring are not decoration, they are what stops this panel from lying.
`stale_seconds` sets the threshold; `scope.stats` reports `age_seconds` and
`stale` per row, so it is checkable without a screenshot.

### Which instant "now" is

The **shared** one: the cursor when there is one, otherwise the view's right edge
— which is the source's clock live and the playhead on a recording. That is the
whole reason the cursor is shared, and a table is where it pays off most, because
a table beside a plot beside a video frame is only one coherent picture if all
three answer for the same instant.

The value is the newest sample **at or before** that instant, held rather than
interpolated. A reading between two samples is a number nothing published, and
for a state it is not even wrong — it is a fractional ordinal that names nothing.
`follow_cursor: false` reads the newest sample instead, which is what you want on
a live bus with a cursor left parked from an earlier look.

### Resizing the columns

Grab the line between two columns and pull. The cursor turns into a split arrow
over a divider, and **double-clicking one hands that column back to sizing
itself** — which is the only discoverable way back from a drag you regret, since
the value that means "automatic" is a sentinel no user would guess.

The three sized columns pack against the **right** edge and the name column
absorbs whatever they leave. That is what makes a drag mean one thing: a divider
moves its own column's left edge, and the name column gives up or takes back the
difference. There is no `name_width` in the config for the same reason — it has
no width of its own, and giving it one would be a second way to describe the same
layout, with no rule for which wins.

`value_width` / `units_width` / `age_width` are pixels, and **-1 means
automatic** — the same sentinel `decimals` uses on a row. A column nobody has
touched keeps sizing itself to the panel; one that was dragged stays where it was
put. A plain default cannot express both: it would freeze every column at a
number chosen in the source, which is exactly what left `airplayHandshake`
elided inside a panel with 500 px of empty space in it.

Pixels rather than fractions, because pixels are what was dragged. A fraction
survives a resize more gracefully and gets the important case backwards: a column
sized to fit `12 ms` would stop fitting it the moment the dock narrowed. Widths
are clamped against the panel at paint time instead, so a narrow dock squeezes
rather than overflows — and the name column keeps a minimum whatever the others
ask for, because rows of readings with nothing saying what they are readings *of*
are worse than a value that elides.

A drag stores **what the panel will actually show**, not what the pointer asked
for. Without that second step a drag past a clamp would save a number that was
never drawn, and reloading the workspace would appear to move the columns by
itself.

### Editing a panel keeps the data it already has

**A config change rebinds only the signals that actually changed.** Identity is
the binding triple — key, schema, expression — which is exactly what `SignalKey`
is built from, so two rows the source cannot tell apart are two rows this treats
as the same. A label, a colour, a units suffix, a format, a decimal count, an
axis, a column width: all presentation, all keep the buffer.

This started as a **data-loss bug**, reported from a live session and worth
recording because nothing about it looked like a failure:

> Attached Live with a few signals reading correctly. Paused, then added one more
> signal — and every row in the table went to `--`. Resuming brought the values
> back, but only from the resume point forwards; everything before it stayed
> blank.

`addBinding()` rebuilt *every* row, so the existing ones got brand-new empty
buffers and their history was gone. While the view is paused the readout instant
is frozen in the past, and a fresh buffer has nothing at or before it — so every
row correctly reported "nothing here" and the panel looked dead. Resuming moved
the instant to `now()`, where the refilled buffers did have samples. The stretch
before the resume was simply gone, because the history covering it had been
discarded.

Nothing logged it. From the panel's point of view it had just bound successfully,
three times.

**The plot had the identical bug** — same cause, and there it blanks every line
already drawn instead of every cell. Its class header had promised the correct
behaviour ("signals that are unchanged keep their history rather than being torn
down and restarted") since it was written, while `applyConfig()` did the
opposite.

Two cases still rebuild everything, because a buffer genuinely cannot be carried
over: `rebindTo()` (a different source issued the handles) and
`setHistorySeconds()` (the retention a buffer was constructed with changed). Both
go through `rebindAll()`, and both are honest about the loss — a buffer cannot
grow a past it never recorded.

### Taking a binding back

Adding the table exposed a hole in `Panel`: `addBinding()` was virtual from the
start and **removing one was not**. Both the context menu and
`scope.remove_signal` cast to `TimeSeriesPanel`, so "Remove signal" was silently
missing from every other panel kind — including the video panel, which had a
`removeStream()` that nothing ever called — and the RPC answered *"that panel has
no removable signals"* about a binding that was plainly there.

`bindingLabels()` and `removeBinding()` are now on `Panel` beside
`addBinding()`, and `scope.panels` reports `bindings` for every panel whatever
kind it is. `scope_test_panels` drives both over **every** entry in
`availablePanelTypes()`, so the next panel cannot reintroduce the hole.

## The map panel

Where the vehicle went, under the shared clock.

```bash
./build/scope/scope --bag drives/2026-08-28
# File ▸ Settings… to point 'socal' at an .mbtiles, then
# Panels ▸ Add ▸ Map, and drag the nodes/bd992/gsof/lat_long_height TOPIC onto it
```

A plot answers *what was the speed at this moment*. A map coloured by speed
answers *where on the lap was I slow*, which is the question a plot is worst at
and the reason to want one beside it.

### No map_server, and no zenoh session for tiles

The dashboard's map widget asks `nodes/map_server` over the bus. This one opens
the `.mbtiles` itself. Nothing has to be running: `scope --bag` and a path in
Settings is the whole setup.

That also removes a trap. Two `map_server`s on one zenoh key both answer and the
client takes whichever is first, so a screenshot has previously "proved" the
wrong archive rendered. Scope opens no session for tiles, so a screenshot here
is evidence about the file named in Settings and nothing else.

What IS shared with the dashboard is everything that turns a vector tile into
pixels — projection, tessellator, GPU renderer, label layout, tile cache. That
is `libs/map_render`, hoisted out of `dashboard/widgets/map` for exactly this.
The panel itself — camera, tile selection, paint driver, config, interaction —
is its own, the same call `scope/panels/video/video_decoder` made against
`CarPlayWidget`'s decoder.

The local path is simpler than the served one in three ways, and each is a whole
mechanism that does not exist here: no backoff or retry timer (a read succeeds,
is absent, or fails — there is no server that might come up later), no
zoom-range learning (`minzoom`/`maxzoom` is known when the file opens), and no
`outOfRange` (the panel clamps before asking, so such a coordinate is never
formed).

### It takes a topic, or three fields

Dropping the position **topic** fills latitude and longitude in one go, from a
small table of `{schema → lat field, lon field}` covering `GsofLatLongHeight`,
`GsofLatLongMslHeight`, `GsofCodePosition`, `GsofInsFullNav` and
`CarPlayLocation`. A schema not in that table is **declined at the drop** rather
than bound and then silently drawing nothing.

Field drops fill the first empty role — latitude, then longitude, then colour —
and `bindingLabels()` names the role, because unlike a plot's traces these three
are not interchangeable and "remove the second one" would otherwise be a guess.

**Longitude must be on the same topic as latitude**, and a drop from elsewhere is
refused. Every position schema in this tree carries the pair on one message, and
the source stamps one timestamp per message which every binding on it shares —
that shared timestamp is the only thing they pair on. A longitude from another
topic would bind cleanly and then draw nothing.

### Pairing, and the counter that explains an empty map

Positions are built by matching a latitude and a longitude **at the same
instant**, never by index. Index pairing skews the whole track the moment one
binding is added after the other, or one sample is dropped — and the line is
still a line, over the wrong roads.

`scope.stats` reports `paired_points` alongside `unpaired_latitude` and
`unpaired_longitude`, and that pair of numbers is the whole diagnostic: thousands
of latitudes with zero paired points means the two signals are not on one topic.
On screen that is an empty panel, indistinguishable from no data at all.

### The trail is the retention window

Not the whole recording: exactly what the plots hold, `history_seconds`, ending
at the playhead. Raising `history_seconds` is how you see a whole lap at once.

The stretch inside the current view is drawn solid and wider; everything else is
dimmed to `track_opacity`. **That band is the only thing on screen relating the
map to the time base** — without it the panel is a picture beside the plots
rather than a view of the same window.

### The marker reads the shared instant

The cursor when there is one, otherwise the view's right edge — which is the
source's clock live and the playhead on a recording. Identical to the table's
rule, and it must stay identical or the marker and the table row disagree about
the same moment. `marker_t` reports it, because a marker drawn for the wrong
instant looks exactly like a correct one.

### Clicking the track moves the shared cursor

The reason a map belongs in a review tool, and the one direction the dashboard
widget has no equivalent of. A click within `click_radius_px` of the drawn track
moves the **shared** cursor to that point, so every plot, table and video frame
jumps to that corner.

A press either grabs the track or the map, decided once at press time and not
revisited — a drag that changed its mind halfway feels like a bug. Clicks away
from the track pan instead. A drag is bracketed with `setInteracting()`, or a
mouse-move would refill every signal's whole retention window per event.

If the point is outside `[view_begin, view_end]` the panel also seeks: on a
recorded source the view's right edge *is* the playhead, so a cursor parked
outside the window would mark an instant no plot is drawing.

Nearest rather than first-within-radius, because a lap crosses itself and two
points sit under the pointer — picking whichever came first in the buffer seeks
to the wrong lap, and both are on the track, so nothing about the result looks
wrong.

### Colouring the trail

Bind a third signal and the trail takes a ramp along its length. `viridis` by
default (perceptually uniform, colour-blind safe, dark at the low end — which
matters on a dark basemap), `turbo` for spotting extremes, `gray` for when the
map underneath is already carrying colour.

The colour at each point is the newest sample **at or before** it, held rather
than interpolated: an interpolated value is a number nothing published, and on a
corner it paints a speed the car never did.

`color_autoscale` fits the ramp to the range actually present; a constant signal
gets a widened range rather than a division by zero, which would paint NaN and
therefore nothing.

### Diagnosing an empty panel

Every one of these is a different fault and on screen they are the same picture,
so the panel captions itself and `scope.stats` reports the same string in
`diagnostic`:

| Message | Means |
|---|---|
| `Tileset 'socal' is not configured — File ▸ Settings…` | the name in the panel config is not in Settings |
| `'socal' could not be opened: …` | it is configured, and the archive will not open |
| `No position bound — drag a position topic onto this panel` | nothing to draw |
| `Latitude and longitude never share a timestamp` | they are on different topics |
| `No GPU backend` | QRhi found none; trail and marker still draw |
| `Reading tiles…` | requests are out, nothing back yet |
| `No coverage here in 'socal'` | tiles arrived and the archive is empty here |

Empty means nothing is wrong, which is the assertion worth making.

A ready-made layout is in `configs/scope/drive_review.yaml` — the map and a
speed trace over one clock:

```bash
./build/scope/scope --config configs/scope/drive_review.yaml --bag drives/today
```

### Driving it

```python
app_call("scope.settings", {"tilesets": [
    {"name": "socal", "path": "/Users/ryan/Documents/map_data/socal.mbtiles"}]})
app_call("scope.open_recording", {"path": "/tmp/drive"})
scope_add_panel(type="map", id="map1")
app_call("scope.panel_set_config", {"panel": "map1", "config": {"tileset": "socal"}})
# Topic-level: fills latitude AND longitude.
scope_add_signal(panel="map1", zenoh_key="nodes/bd992/gsof/lat_long_height", field="")

app_call("scope.stats", {"panel": "map1"})
#   paired_points > 0 with unpaired_* == 0   -- the track is real
#   tiles_decoded > 0                        -- the archive is being read
#   diagnostic == ""                         -- nothing is being captioned
```

`input_click` on the panel is what exercises the seek. Note that **Follow Cursor
moves the camera after the first click**, so a series of clicks at fixed
coordinates lands on a shifted map — pin the camera first when asserting on more
than one:

```python
app_call("scope.panel_set_config", {"panel": "map1", "config": {
    "follow_cursor": False, "center_latitude": 33.6853,
    "center_longitude": -117.8561, "zoom": 15.5}})
input_click(target="…/scope::MapPanel", x=400, y=170)
app_call("scope.time_base", {})     # cursor moved to that point's time
```

The target has to be the **panel**, not its dock: the dock carries the panel's
`objectName`, so `target="map1"` resolves to the dock and a click lands on its
title bar. `ui_snapshot` gives the panel's path.

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

## Settings

A workspace says *what to show*. Settings say *where things are on this
machine*. The two are separate files, and the dividing line is worth stating
plainly because getting it wrong is what makes a shared workspace stop opening:

| | Workspace | Settings |
|---|---|---|
| holds | panels, bindings, layout | map archive paths |
| lives in | `configs/scope/`, wherever you point `--config` | the platform's per-user config location |
| meant to be | committed, shared, opened on someone else's laptop | never shared; it is about one computer |

**A map panel names a tileset, never a path.** `tileset: socal` is what a
workspace carries; settings say that `socal` is
`/Users/ryan/Documents/map_data/socal.mbtiles`. Put a path in a workspace and it
opens nowhere but the machine that wrote it.

Where the file lives:

```
Linux   ~/.config/redline/scope/scope.yaml
macOS   ~/Library/Preferences/redline/scope/scope.yaml
```

From `QStandardPaths::AppConfigLocation`, so it follows whatever the platform
says. `--settings <path>` overrides it — which is not a convenience: without it
every `ctest` run and every `--mcp` run would read and write the *developer's*
real settings, so a test that adds a tileset would leave it behind and one that
clears them would take the user's away.

```yaml
tilesets:
  - name: socal
    path: /Users/ryan/Documents/map_data/socal.mbtiles
  - name: tracks
    path: /Users/ryan/Documents/map_data/tracks.mbtiles
```

The same file carries the per-machine conveniences — `recent_workspaces` and
`recent_recordings` (the File menu's Recent submenus), `last_directory` (where
the file dialogs open), and `window_geometry` — because each is a fact about
this machine, which is exactly the dividing line above. All are safe to delete.

Three behaviours are deliberate, and each is a case where the obvious
alternative fails quietly:

- **A missing file is not an error.** It is a first run. Nothing is written
  until something changes — an app that creates a file merely by starting is
  surprising, and reporting a problem on a clean machine trains people to ignore
  the report.
- **A malformed file is a warning plus defaults, and is NOT overwritten.** A
  hand-edit with a typo in it is worth more than the empty file that would
  replace it. You see the warning and your file is still there.
- **Writes go through a temporary and a rename.** A truncated settings file
  reads back as "no tilesets configured", which is indistinguishable from a user
  who configured none.

A duplicate name, a name containing `/`, an unnamed entry and an entry with no
path are all reported rather than refused — as log lines, in the dialog, and in
`scope.settings`'s `notes`. `nodes/map_server` refuses the same four at startup;
it can afford to, because it has nothing else to do, and here the equivalent
would be an app that will not open.

### Editing them

**File ▸ Settings…** lists name, path and status. The status column is the point
rather than decoration: it opens the archive and reports either `z0–14 pbf` or
the actual reason it could not, which is the same question `map_server --check`
exists to answer. "Cannot open" is one sentence for a typo, a permissions
problem and a file that is not an archive, and those are three different fixes.

Headlessly, and this is what every test uses:

```
app_call("scope.settings", {})                     # read
app_call("scope.settings", {"tilesets": [
    {"name": "socal", "path": "/Users/ryan/Documents/map_data/socal.mbtiles"}]})
```

Writing replaces the whole list rather than merging — a caller that wanted to
*remove* a tileset has no way to say so through a merge, and a partial write
that silently kept an old entry shows up later as a map drawn from the wrong
archive. The dialog is a second front end onto the same
`ScopeWindow::setSettings()`; it refuses to open under `--mcp`, where a modal has
nobody to dismiss it.

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
| `scope.panels` | panels, their `bindings`, and the types available |
| `scope.add_panel` / `scope.remove_panel` | compose the window |
| `scope.add_signal` / `scope.remove_signal` | bind and unbind, on any kind of panel |
| `scope.browser` | the topic→field tree, optionally rescanning first |
| `scope.browser_drag` | drive the drop path itself |
| `scope.time_base` | the view window: `view` / `pan` / `zoom` / `fit` / `seek`, plus `following`, `window_seconds`, `playing`, `rate`, cursor and caps |
| `scope.density` | what the overview strip draws behind everything else, as numbers |
| `scope.panel_get_config` / `_set_config` / `_describe_config` | reflected config, for whichever kind of panel it is |
| `scope.stats` | what each panel has RECEIVED, whatever kind it is |
| `scope.describe_stats` | what fields `scope.stats` will return for that panel |
| `scope.save` / `scope.load` | workspaces |
| `scope.settings` | the per-user tileset list: read with no params, replace with `tilesets` |
| `scope.source` | `mode` (online/offline), `kind` (live/recorded/empty), and `decodes_pending` |
| `scope.set_mode` | `{"mode": "online"}` attaches to the bus and starts capturing; `"offline"` detaches and lands on the capture |
| `scope.open_recording` | open a bag directory — implies offline |
| `scope.capture` | messages, bytes, retained span, **evicted**; `running: false` once offline |
| `scope.review_capture` / `scope.save_recording` | review the session capture; write it out as a bag |
| `scope.sample_stats` | the time-series half of `scope.stats`, under its historical name |

The loop that closes fastest:

```
app_launch(app="scope")
scope_source(online=True)     # FIRST. A window starts offline and sees no topics.
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

**Panning is the same assertion.** On a recorded source the view's right edge is
the playhead, so a pan must move the buffers too. This is what proves the rule
holds through the gesture path rather than only through `seek`:

```
app_call("scope.time_base", {"window_seconds": 5})
app_call("scope.time_base", {"pan": -10.0})
scope_sample_stats(...)                # t_last moved back by ten seconds
app_call("scope.time_base", {})        # following=false, playing=false
```

**Zoom is checkable without a screenshot too**, and the property to check is the
anchor, not the span:

```
app_call("scope.time_base", {"zoom": {"factor": 0.5, "anchor": 12.0}})
app_call("scope.time_base", {})   # span halved, and 12.0 sits at the same
                                  # fraction of [view_begin, view_end] as before
```

Naming two movers in one call is refused rather than guessed at:
`{"pan": -5, "zoom": 0.5}` is `BAD_PARAMS`. They all move the window, so
composing two produces a result that cannot be read back from the request — and
the caller is usually a model that will then reason from the wrong position.

**And the strip's background, which really cannot be screenshotted usefully:**

```
app_call("scope.density", {"buckets": 10})   # buckets, t_begin/t_end, exact
app_call("scope.capture", {})                # messages over the same span
```

The bucket sum against `capture`'s `messages` is the assertion worth making.
`exact: false` means the source declined to answer cheaply rather than slowly —
a bag counting from its part index — not that there is nothing there.

**And the video panel, where a screenshot is least useful of all.** Every reason
a video panel is black looks the same on screen and they are not the same
problem, so `scope.stats` separates them: nothing published (`received` 0), a
schema this binding skips (`bound` but `received` 0), arriving but never syncing
(`dropped_before_sync` climbing), syncing but failing to decode
(`decode_errors`).

```
app_call("scope.describe_stats", {"panel": "cam"})   # what fields to expect
app_call("scope.stats", {"panel": "cam"})            # and what they say
```

The proof a screenshot *can* make, and the reason `--simulate` is the right
vehicle: its test pattern has a **sweeping box whose position is a pure function
of the frame index**, so the picture is a readable clock.

```
app_call("scope.set_mode", {"mode": "offline"})   # stops the capture, lands on it
app_call("scope.time_base", {"window_seconds": 5})
app_call("scope.time_base", {"seek": 20})
ui_screenshot(target="video_panel")            # note the hash
app_call("scope.time_base", {"seek": 6})       # BACKWARDS
ui_screenshot(target="video_panel")            # a different picture
app_call("scope.time_base", {"seek": 20})      # and back
ui_screenshot(target="video_panel", if_changed_from="<the first hash>")
```

The last call returning `unchanged: true` is the assertion. "There is a picture"
proves nothing; "the same instant gives a pixel-identical picture, after a round
trip through somewhere else" proves the seek.

Note that `{"seek": 20}` with the default 30 s window lands the view at 30, not
20: the window *slides* into the available range rather than being squashed.
Narrow it first, as above, or seek somewhere further in than the span.

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
- `scope_test_time_base` (unit) — the view window: zooming, panning, and what
  clamps it. Almost every way this can be wrong **looks like data** rather than
  like a bug — a zoom that does not hold its anchor drifts the trace under the
  pointer, a pan that leaves the buffers behind draws the wrong stretch of a
  recording, a live view that clamps at the right edge just stops scrolling.
  None of them raise anything. The load-bearing cases are the anchor holding its
  fraction of the window through a zoom, the live-edge re-arm, and that a drag
  produces exactly one seek rather than one per event.
- `scope_test_decimate` (unit) — column reduction against a brute-force
  reference over random windows. Every way this can be wrong looks like data
  rather than like a bug.
- `scope_test_settings` (unit) — the per-user settings codec: a missing file is
  defaults, a malformed one is not overwritten, byte stability, and the name
  checks.
- `scope_test_map_track` (unit) — pairing by timestamp, the unpaired counters,
  colour held rather than interpolated, pixel thinning and the hit test. Every
  case here DRAWS SOMETHING when it is wrong, which is why none of it is left to
  a screenshot.
- `scope_test_map_tiles` (unit) — `TileReader` against archives it writes
  itself: the XYZ/TMS flip (a double flip renders beautifully, mirrored about
  the equator), absent versus failed, an unopenable archive, and the zoom range
  at open. Also reads the real socal archive when present, and skips loudly when
  not.
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
- `scope_test_raw_buffer` (unit) — the same two-bound argument one layer up, plus
  the backwards seek: the assertion is that `t_last` **moved back**, not that the
  buffer is still ordered, because a buffer that kept the window it came from
  stays perfectly ordered and is completely wrong.
- `scope_test_video_decoder` (unit) — the decoder, against a stream the test
  **encodes itself** with the same settings `simulate.cpp` uses. No fixture in
  the tree: a checked-in `.h264` cannot rot against the ffmpeg the build links,
  and the pattern encodes its own frame index so a seek is checked on the pixels.

  Worth reading for what the *simple* stream cannot prove. With
  `max_b_frames = 0` every keyframe is an IDR and the decoder holds nothing back,
  so `reset()` has no observable effect at all — removing
  `avcodec_flush_buffers()` left the whole suite green, which is exactly the
  "test that passes against the bug" this tree warns about. `testReorderedStream`
  encodes a B-frame stream for that reason, and it fails if either the flush or
  the packet-timestamp propagation is removed. Both were checked by reverting
  them.
- `scope_test_workspace` (unit) — the codec, weighted towards hand-edited files
  that are wrong in the usual ways.
- `scope_test_panels` (gui) — the window and panel layer against a real widget
  tree, using a stub `DataSource` so it needs no bus. This one caught a live
  crash: `{"zenoh_key": 42}` in dropped data threw a `json::type_error` out of a
  Qt drop handler, which terminates the app. It also pins the source-swap
  ordering rule, and that `history_seconds` reaches the panels — it round-tripped
  through the YAML perfectly for a year while both ends ignored it.

  The gesture cases live here rather than in `scope_test_time_base` because a
  gesture converts pixels against **what the panel actually drew**, so they need
  a real widget. Synthesised `QWheelEvent`/`QMouseEvent` are delivered fine
  offscreen; it is only `QDrag::exec()` that cannot run there. Note that moving
  the view and then sending a gesture without a repaint in between converts
  against the *previous* window — deliberately, so a click lands on the instant
  the user can see, and a test has to force a paint.

  The assertion that matters most: **a zoom on panel A moves panel B's window.**
  That is the whole requirement; without it the panels have quietly grown
  independent axes and the shared cursor lines up with nothing.

  It also carries the table-driven seam check: **every** entry in
  `availablePanelTypes()` must yield a config and a stats variant whose active
  alternative matches its `panel_type_t`, and both must survive the reflected
  codec. That is what keeps the *next* panel honest — it fails the moment someone
  adds a table row without a stats struct, and the failure it prevents is not a
  crash but `scope.stats` answering `{}`, which looks exactly like a working
  panel that has received nothing.

  The same sweep now runs over `bindingLabels()`/`removeBinding()`: every panel
  kind must accept a candidate, report it under a name a human could pick out of
  a menu, and give it back. That one is there because the hole it closes was
  real and silent for two panel types — see [the table panel](#the-table-panel).

  The table panel's own cases assert what a cell **prints**, not just what it
  holds: `3` renders as `iap2`, `phase * 2` renders as `6`, a sample from *after*
  the readout instant is not read out at all, and a reading older than
  `stale_seconds` is still shown and marked. The value and the text are both in
  `scope.stats` precisely so the mapping between them is assertable.

  Both panels pin the **rebind rule**: adding a signal must not release or
  rebuild the ones already bound. The assertion is on the source — one bind per
  signal, nothing released — because that is what fails loudly if a wholesale
  rebind is reintroduced, and the visible symptom (`--` in every cell, a blank
  plot) only appears when the view happens to be paused.

  Its column geometry is checked against numbers **worked out from the layout
  rules in the test**, not read back from the panel — a test that asked the panel
  where its dividers were and then checked it dragged them there would agree with
  the arithmetic however wrong it was. That is not hypothetical: writing those
  numbers out by hand is what found the value column being measured from the
  units column's *right* edge, so the two overlapped by 42 px whenever units were
  shown. Reintroducing that one-token bug fails eight assertions.

> **One objectName did not survive.** `transport_scrubber` was a `QSlider` and
> `overview_strip` is not one, so keeping the name would make an agent that
> clicks it and then sets a value fail in a way that looks like a broken app
> rather than a renamed widget. `scope_test_panels` pins that it is gone rather
> than quietly re-pointed at something else. Everything else on the transport
> bar kept its name, including `transport_status`, which moved to the top bar —
> a stable name is worth more than a tidy prefix.

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
