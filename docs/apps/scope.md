---
title: scope
parent: Apps
redirect_from: /scope.html
---

# scope

## Overview

`scope` is the live time-series visualizer. `dashboard` shows what a value
*is*; `scope` shows what it has been *doing*. It plots, tabulates, maps and
replays the same zenoh messages the dashboard consumes, over one shared clock,
so a value read off a plot lines up with the table row, the map marker and the
video frame beside it.

It has two kinds of source. Online, it tails the live bus and records the
whole of it into memory as it goes. Offline, it reads a recording: a bag on
disk, or the capture it made while it was online. Panels do not know which is
behind them; the difference is whether you can seek.

The reasoning behind the design, the buffering and the per-panel decisions is
in [scope internals](../design/scope-internals.html). This page is how to use
it.

## Running it

```bash
cmake --build build --target scope
./build/apps/scope/scope                                     # offline and empty, the default
./build/apps/scope/scope --bag drives/2026-08-06             # offline, over a recording
./build/apps/scope/scope --online                            # attach to the bus and capture
./build/apps/scope/scope -c configs/scope/engine_demo.yaml   # a saved workspace
./build/apps/scope/scope --mcp                               # headless, agent-driven
```

| Option | Meaning |
|---|---|
| `-c`, `--config <path>` | YAML workspace to open. Omit to start empty. |
| `-b`, `--bag <dir>` | Bag directory to open. Implies offline. |
| `--online` | Attach to the bus and start capturing at startup. |
| `--settings <path>` | Per-user settings file. Omit to use the platform's config location. |
| `--mcp[=<socket>]` | Agent control interface on a unix socket, headless (Qt platform forced to `offscreen`). Default `/tmp/redline_scope_<pid>.sock`. |
| `--debug` | Debug logging. |
| `-h`, `--help` | Usage. |

Scope starts offline and does not open a zenoh session until you ask it to.
Load a bag to scrub through, or press `Offline` on the toolbar to go online and
watch the live bus. `--online` is the startup form; it is mutually exclusive
with `--bag`, because a bag is an offline source and asking for both asks for
two different things. Start with `--bag` and go online from the toolbar, or use
`scope.open_recording` once online.

{: .warning }
`--mcp` takes its value with `=`, as in `--mcp=/tmp/agent.sock`. A
space-separated path is reported as an unrecognised argument and scope exits
rather than listening somewhere other than where you asked.

There is nothing to see online without traffic on the bus. For a car-free
bring-up:

```bash
./build/mock_data/test_data_publisher &
./build/apps/scope/scope -c configs/scope/engine_demo.yaml --online
```

Per-user settings live in the platform's config location, from
`QStandardPaths::AppConfigLocation`:

```
Linux   ~/.config/redline/scope/scope.yaml
macOS   ~/Library/Preferences/redline/scope/scope.yaml
```

The file holds the map tileset list, `recent_workspaces` and
`recent_recordings` (the File menu's Recent submenus), `last_directory` (where
the file dialogs open) and `window_geometry`. All are facts about this machine
and all are safe to delete. A missing file is a first run, not an error, and
nothing is written until something changes. A malformed file is a warning plus
defaults and is not overwritten, so a hand-edit with a typo in it is still
there to fix.

```yaml
tilesets:
  - name: socal
    path: /Users/ryan/Documents/map_data/socal.mbtiles
  - name: tracks
    path: /Users/ryan/Documents/map_data/tracks.mbtiles
```

{: .tip }
Pass `--settings` to every test and `--mcp` run. Without it, a test that adds
a tileset leaves it in your real settings and one that clears them takes yours
away.

## Workspaces

A workspace says what to show; settings say where things are on this machine.
The two are separate files, and the dividing line is what keeps a shared
workspace opening on someone else's laptop.

| | Workspace | Settings |
|---|---|---|
| holds | panels, bindings, layout | map archive paths, recents, geometry |
| lives in | `configs/scope/`, or wherever you point `--config` | the platform's per-user config location |
| meant to be | committed, shared, opened on someone else's laptop | never shared |

Workspaces are YAML under `configs/scope/`, hand-editable and diffable. Two
ship with the tree: `engine_demo.yaml` (two plots over the mock publisher's
engine and chassis topics) and `drive_review.yaml` (a map and a speed trace
over a GNSS recording). The top level carries the window-wide settings and the
panel list:

```yaml
name: Engine Demo
history_seconds: 300            # retained per signal; also the map trail's length
window_seconds: 30              # shown at once
render_rate_hz: 30
max_capture_bytes: 1073741824   # in-memory capture bounds while online; 0 disables
max_capture_seconds: 1800
panels:
  - id: engine
    type: time_series
    config:
      title: Engine
      traces:
        - zenoh_key: vehicle/engine/rpm
          schema_type: EngineRpm
          value_expression: rpm
          label: rpm
          units: rpm
dock_state: ""
```

`File ▸ Open Workspace…`, `File ▸ Save Workspace` and the `Open` / `Save`
toolbar buttons are the same actions; `Open Recent Workspace` lists the last
few. `dock_state` is the dock arrangement as an opaque, Qt-versioned blob
stored alongside the readable half. A failed restore is a warning and a
default layout, and losing the blob costs a re-drag, not data. Leave it empty
in a committed file so the file is not tied to a Qt version.

Every panel needs an `id:`; it is the dock's name, and a panel without one is
warned about at load because its saved position cannot be matched back to it.
An unknown `type:` is a warning, not an error: a workspace written by a newer
build may name a panel this one has never heard of, so the panel is skipped
and said so rather than the file refused.

The second panel splits below the first; the third tabs with the second.
Rearrange and save if you want a different layout kept.

{: .note }
A hidden tab does not paint, so a panel on an inactive tab reports zero in its
stats until you switch to it.

## Panels

Add a panel from `Panels ▸ Add` or the toolbar's Add buttons. Right-clicking a
panel gives `Configure…`, `Add signal…`, `Remove signal` and `Close panel`;
the dock's X does the same as Close panel. `Configure…` is one
reflection-driven dialog for every panel kind, so every field below is
editable there, and an out-of-range value shows its clamped self after Apply.
Editing a colour, a label or an axis never discards a trace's history; only a
change to a binding's key, schema or expression rebinds that one signal.

Signals come from the `Signals` browser (`View ▸ Signals`, or the toolbar
button): a tree of topics and their fields. Drag a field or a topic row onto a
panel, or double-click it to put it on the first panel that will take it. Each
panel kind accepts a different shape of thing, and a drop a panel declines is
refused at the drop rather than bound and then silently drawing nothing.

Every panel reports what it has received, as opposed to what it was told to
show, through `scope.stats`; `scope.describe_stats` lists the fields for a
given panel. The per-panel sections below say which numbers matter.

### Time series

Traces over the shared window, decimated to one min/max span per pixel column
so spikes stay visible however far you zoom out. Two vertical axes: engine rpm
and oil pressure differ by three orders of magnitude, and on one scale the
smaller is a flat line whose shape you cannot read. `right_axis: true` on a
trace scales it against the right, and the legend marks it `[R]`.

It takes numeric fields, or expressions over them (`rpm / 1000`,
`mps_to_mph(speedMps)`, `values[7]`), and declines whole topics. A list field
expands into one draggable row per element when its schema declares a fixed
length; a list without that annotation is not plottable element by element and
the browser says so.

An enum or a bool trace is drawn as a lane rather than a line: a band per
state, full width, with the state's name written in it when the band is wide
enough. An enum's ordinals are labels, not quantities, so a line sloping
between two states would draw a transition that did not happen, and a bool as
a line reads as a signal spending half its time at 0.5. Lanes sit under the
numeric traces and do not stretch the value axis. `display` on a trace is
`automatic`, `line` or `lane`; the override matters both ways, since plotting
gear against rpm is legitimate and so is forcing a lane onto a small integer
that is really a state. Only a bare field (`phase`) or one list element
(`values[7]`) resolves to names; `phase * 2` is a number and stays a line.

| Field | Default | Meaning |
|---|---|---|
| `title` | `Plot` | Panel title |
| `follow_time_base` | `true` | Scroll and pause with the rest of the window |
| `window_seconds` | `30` | Seconds shown, used only when not following the shared time |
| `autoscale_y` | `true` | Fit the vertical axis to the visible data |
| `y_min`, `y_max` | `0`, `100` | Axis range when autoscale is off |
| `show_grid`, `show_legend` | `true` | Grid behind the traces; legend with current values |
| `traces[]` | | `zenoh_key`, `schema_type`, `value_expression`, `label`, `color`, `units`, `right_axis`, `display` |

Every gesture moves the one window every panel shares:

| Gesture | Does |
|---|---|
| wheel | zoom about the pointer; the instant under it keeps its place |
| shift + wheel | zoom the value axis; turns autoscale off for that panel |
| drag | pan |
| shift + drag | rubber-band a range and zoom to it |
| double-click | fit all, and give autoscale back |
| `Ctrl` `+` / `Ctrl` `-` / `Ctrl` `0` | zoom in / out / fit, anywhere in the window |
| `←` `→` `space` | pan back / pan forward / follow-or-play, except while the signal tree has focus |

The keyboard shortcuts live in the `View` menu. Panning further right than
there is data on a live source re-arms following, so the plot resumes
scrolling rather than pinning at the live edge.

`scope.stats` reports, per trace: `bound`, `retained`, `received`, `dropped`,
`lane`, `has_data`, `t_first`, `t_last`, `min`, `max` and `last`. `dropped`
above zero means the trace is lying about the data; `lane` says which way an
`automatic` trace went.

### Video

Renders the CarPlay H.264 stream at the shared time base's position, so the
picture and the value under the cursor beside it are the same instant.

```bash
./build/nodes/carplay/carplay --config configs/carplay/carplay.yaml --simulate &
./build/apps/scope/scope
# Panels ▸ Add ▸ Video, then drag the nodes/carplay/video TOPIC row onto it
```

It takes the topic, not a field of it: topic-level drops only, schema
`CarPlayVideo`, one stream per panel. A short scrubber along the bottom drives
the shared time base and shows the seek points as tick marks; it is hidden
when the source is not seekable. On a recording a seek loads one keyframe
window and decodes forward to the instant asked for, spread over two or three
frames rather than stalling the window.

| Field | Default | Meaning |
|---|---|---|
| `title` | `Video` | Panel title |
| `zenoh_key` | | Topic carrying the stream; empty until one is bound |
| `retention_seconds` | `60` | Seconds of encoded video buffered on a live bus (minimum 1) |
| `max_buffer_bytes` | `268435456` | Encoded video held in memory; `0` disables the byte bound (minimum 4 MiB) |
| `show_scrubber` | `true` | The panel's own seek bar |
| `hardware_decode` | `false` | Decode on the GPU. Measured slower than software for scrubbing, since a seek decodes a whole GOP; falls back to software if the GPU refuses the stream |

Every reason a video panel is black looks the same on screen, so `scope.stats`
separates them: nothing published (`received` 0), a schema this binding skips
(`bound` but `received` 0), arriving but never syncing (`dropped_before_sync`
climbing), syncing but failing to decode (`decode_errors`). `decoder` names the
backend in use; `software` on a machine with a GPU decoder means the stream was
refused and the fallback took over. `frame_t` is the source-clock time of the
picture on screen. `decoded` climbs far faster than `presented` on a recording,
because reaching an instant decodes a whole GOP and shows only its last frame.

### Table

A readout of what every bound signal is doing right now, one row each. A plot
answers *what has this been doing*; forty signals of that is forty traces
nobody can read. This answers *what is everything at, this instant*, the
question a pit wall asks and the one a plot is worst at.

```
  Signal          │        Value │    Age
  ────────────────┼──────────────┼───────
  phase           │ airplayHands…│  12 ms
  micActive       │         true │  12 ms
  rpm             │         6120 │   4 ms
  oilPressurePsi  │           45 │  3.1 m   ← stale, and coloured for it
```

It takes exactly what a plot takes, so the same drag fills both. An enum reads
as its name: a lane only has room for the word when the band is wide enough,
and a cell always does. `format` per row is `automatic` (state names for an
enum or a bool, a number otherwise), `number`, `hex` or `state`; `hex` is for
the status words where the decimal form says nothing about which bits are set.

"Now" is the shared instant: the cursor when there is one, otherwise the view's
right edge, which is the source's clock live and the playhead on a recording.
The value is the newest sample at or before that instant, held rather than
interpolated. `follow_cursor: false` reads the newest sample instead, which is
what you want on a live bus with a cursor left parked from an earlier look.

The age column is what stops this panel from lying. A plot shows a line
stopping; a table shows a dead publisher's last value in the same typeface as
a live one, so a reading older than `stale_seconds` is dimmed and marked.

Grab the line between two columns and pull to resize the column to its right;
double-clicking a divider hands that column back to sizing itself. The three
sized columns pack against the right edge and the name column absorbs
whatever they leave. A narrow dock squeezes the columns rather than
overflowing, and the name column keeps a minimum.

| Field | Default | Meaning |
|---|---|---|
| `title` | `Table` | Panel title |
| `follow_cursor` | `true` | Read at the shared cursor; off reads the newest sample |
| `show_age` | `true` | Age column |
| `stale_seconds` | `2.0` | A value older than this is dimmed and marked stale |
| `show_units` | `true` | Units column |
| `value_width`, `units_width`, `age_width` | `-1` | Column widths in pixels (24 to 400); `-1` is automatic |
| `rows[]` | | `zenoh_key`, `schema_type`, `value_expression`, `label`, `units`, `format`, `decimals` (`-1` picks a width from the magnitude) |

`scope.stats` reports `readout_t` and `at_cursor` for the panel and, per row,
`bound`, `retained`, `received`, `dropped`, `has_value`, `value`, `text`,
`state`, `sample_t`, `age_seconds` and `stale`. `text` is what the cell prints,
so `3` rendering as `iap2` is assertable.

### Map

Where the vehicle went, under the shared clock. A plot answers *what was the
speed at this moment*; a map coloured by speed answers *where on the lap was I
slow*.

```bash
./build/apps/scope/scope --bag drives/2026-08-28
# File ▸ Settings… to point 'socal' at an .mbtiles, then
# Panels ▸ Add ▸ Map, and drag the nodes/bd992/gsof/lat_long_height TOPIC onto it
```

Nothing else has to be running. The panel opens the `.mbtiles` itself; there
is no `map_server` here and no zenoh session for tiles. A map panel names a
tileset, never a path: `tileset: socal` is what the workspace carries, and
`File ▸ Settings…` says that `socal` is
`/Users/ryan/Documents/map_data/socal.mbtiles`. The dialog lists name, path
and status, where status opens the archive and reports either `z0–14 pbf` or
the reason it could not, the same check `map_server --check` makes. A
duplicate name, a name containing `/`, an unnamed entry or an entry with no
path is reported in the dialog and the log rather than refused.
`overlay_tilesets` draws extra archives over the base one, such as the
`tracks` archive of circuit outlines; a tileset that is not configured is
captioned, not fatal.

Dropping the position topic fills latitude and longitude in one go, for
`GsofLatLongHeight`, `GsofLatLongMslHeight`, `GsofCodePosition`,
`GsofInsFullNav` and `CarPlayLocation`; a schema outside that set is declined
at the drop. Field drops fill the first empty role, latitude, then longitude,
then colour, and `Remove signal` names the role. Longitude must be on the same
topic as latitude, and a drop from elsewhere is refused: positions are built by
matching a latitude and a longitude at the same instant, and the shared
timestamp of one message is the only thing they pair on.

The trail is the retention window, `history_seconds`, ending at the playhead,
not the whole recording; raising `history_seconds` is how you see a whole lap
at once. The stretch inside the current view is drawn solid and wider, and
everything else is dimmed to `track_opacity`. The marker reads the shared
instant by the table's rule. Clicking within `click_radius_px` of the track
moves the shared cursor to the nearest track point, so every plot, table and
video frame jumps to that corner, and seeks if the point is outside the
current view; clicks away from the track pan the map. `Follow Cursor` keeps the
camera on the marker, and a manual pan or zoom overrides it for the session.

Bind a third signal and the trail takes a colour ramp along its length:
`viridis` by default (perceptually uniform, colour-blind safe, dark at the low
end, which matters on a dark basemap), `turbo` for spotting extremes, `gray`
for when the map underneath is already carrying colour. The colour at each
point is the newest sample at or before it, held rather than interpolated.
`color_autoscale` fits the ramp to the range present; a constant signal gets a
widened range rather than an empty one.

| Field | Default | Meaning |
|---|---|---|
| `tileset` | | Name from Settings; empty draws no basemap |
| `overlay_tilesets` | `[]` | Extra archives drawn over the base |
| `min_zoom`, `max_zoom` | `0`, `17` | Camera zoom limits (0 to 22) |
| `center_latitude`, `center_longitude`, `zoom` | Irvine, `14` | Where the camera sits before any position arrives |
| `interactive` | `true` | Drag to pan, wheel to zoom |
| `follow_cursor` | `true` | Keep the camera on the marker |
| `orientation`, `bearing` | `north_up`, `0` | `course_up` turns the map so the drive's direction points up; the compass button cycles this |
| `view_mode`, `pitch` | `top_down`, `45` | `perspective` tilts the view back by `pitch` degrees; the view button toggles this |
| `click_seeks`, `click_radius_px` | `true`, `12` | Clicking the track moves the shared cursor |
| `latitude`, `longitude`, `color_by` | | Each is `zenoh_key`, `schema_type`, `value_expression` |
| `color_ramp`, `color_autoscale`, `color_min`, `color_max`, `show_color_legend` | `viridis`, `true`, `0`, `100`, `true` | The trail ramp |
| `track_color`, `track_width`, `track_opacity`, `view_track_width` | `#FF3B30`, `3`, `0.35`, `4.5` | The trail |
| `marker_color`, `marker_size`, `marker_outline_color` | `#FFFFFF`, `7`, `#101216` | The marker |
| `style` | | Colours and widths for the map itself |
| `show_status` | `true` | Caption an empty map with the reason |

`scope.stats` reports `paired_points` with `unpaired_latitude` and
`unpaired_longitude`, and that pair of numbers is the whole diagnostic:
thousands of latitudes with zero paired points means the two signals are not
on one topic. `tiles_requested`, `tiles_decoded`, `tiles_absent`,
`tiles_failed`, `tiles_drawn` and `tiles_stand_in` say whether the archive is
being read (`absent` is expected and common, `failed` is a fault, and a
`stand_in` count that stays non-zero means tiles are not arriving).
`marker_t`, `marker_latitude`, `marker_longitude`, `camera_*` and `gpu_ready`
describe the last frame. `diagnostic` carries the caption the panel is
painting on itself, empty when it paints none:

| Message | Means |
|---|---|
| `Tileset 'socal' is not configured — File ▸ Settings…` | the name in the panel config is not in Settings |
| `'socal' could not be opened: …` | it is configured, and the archive will not open |
| `No position bound — drag a position topic onto this panel` | nothing to draw |
| `Latitude and longitude never share a timestamp` | they are on different topics |
| `No GPU backend` | QRhi found none; trail and marker still draw |
| `Reading tiles…` | requests are out, nothing back yet |
| `No coverage here in 'socal'` | tiles arrived and the archive is empty here |

`configs/scope/drive_review.yaml` is a ready-made layout, the map and a speed
trace over one clock:

```bash
./build/apps/scope/scope --config configs/scope/drive_review.yaml --bag drives/today
```

## Discovery

Topics appear in the picker as soon as a node starts, whether or not it has
ever published. There is no rescan, no polling for traffic and no window to
wait for: every publisher declares a zenoh liveliness token when it is
constructed, and scope watches that key space with history, so a browser
opened after the nodes still sees all of them. On a recording the same tree
comes from the file's index.

A topic whose publisher goes away is greyed, never evicted. A row you may have
bound must not vanish underneath you, and an unreachable publisher and a
crashed one are indistinguishable from here.

What the picker deliberately does not tell you is that data is flowing. A
token is up while its process is, so a CAN bridge with an unplugged adapter
still advertises its topics. The topic exists and is bindable;
`scope.sample_stats` is where you find out whether anything is arriving.

An empty picker on a fresh window means scope is offline, which is the
default: it has opened no zenoh session and has nothing to list. Go online, or
load a recording. An empty picker while online means no publisher is up on the
bus scope can reach.

Topic keys are `[A-Za-z0-9_-/]` with no leading or trailing `/` and no empty
segments. Config load reports a key outside that charset as an error with the
field path, for example `widgets[0].config.zenoh_key`, and refuses to load.
The reasons are under [topic key rules](../design/scope-internals.html#topic-key-rules).

## Recording and replay

While it is online, scope records the whole bus into memory, with no
exclusions, so a signal nobody thought to plot can still be added afterwards.
Offline it records nothing and opens no zenoh session at all.

The capture is bounded by bytes and time, whichever binds first, evicting
oldest. Defaults are 1 GiB and 30 minutes, both in the workspace
(`max_capture_bytes`, `max_capture_seconds`; `0` disables either). CarPlay
streaming runs about 1.5 GB/hour, where the byte bound bites; telemetry alone
is about 11 MB/hour, where only the time bound does. Eviction is counted and
shown next to the retained span in the transport bar, and a saved capture
records it as the bag's `dropped_messages`.

{: .important }
The capture is a snapshot of the online session, not a tail. It starts when
you go online and stops when you go offline, so the interval you spend
scrubbing is not captured. Go back online before you need the next event.

The top bar is about mode and composition; the bottom bar is about time.

```
[Offline ▾][Load Recording…]  drives/2026-08-10 · 412 s   ∿ Time Series   − + ⤢ Fit   Open Save   Signals
[● Online ▾][Load Recording…]  ⏺ capturing · 41k messages  ∿ Time Series   − + ⤢ Fit   Open Save   Signals
```

The `Offline` / `● Online` button reads the state it is in and toggles it. Its
dropdown holds `Load Recording…`, `Review Session Capture` and `Save
Recording…`, which are also in the `File` menu beside `Recent Recordings`. The
chip beside it says what is behind the panels: the bag and its duration, the
session capture, or `nothing loaded`. Going offline lands you on the capture
you made, `Review Session Capture` returns to it after a bag, and the capture
is dropped only when a new online session starts, after an unsaved-capture
prompt. Saving goes through `bag::BagWriter`, so the result is an ordinary
bag: `bag info`, `bag verify`, `bag play` and Foxglove all work on it. Opening
a recording that lacks some of the workspace's topics reports how many signals
are not in it.

The bottom bar is the overview strip: the whole recording at a glance, with
the view window drawn on it as a region you can grab. Back to front it draws
the message-density histogram, the stretch the panels' buffers can reach, the
eviction head, the view window with a grab handle on each edge, the playhead
and the shared cursor. Drag the body to pan, drag an edge to zoom, click
outside to jump, wheel to zoom about the pointer. The histogram is exact for
the in-memory capture and approximate for a bag on disk, which is counted from
its part index, so a single-part recording draws as one flat block.

On a recording the view's right edge is the playhead, and every panel shares
the one window. A seek moves the whole window so its right edge lands where it
was told, and a pan or zoom seeks to the new right edge. `following` means the
right edge is being driven by something other than you, the source's clock
live or playback on a recording, and every pan and zoom clears it. Zoom-out
stops at what is retained: the recording's extent, or the last
`history_seconds` on a live source. The window slides into range rather than
being squashed, so a seek to `20` with the default 30 s window lands the view
at `30`; narrow the window first if you want the edge exactly there.

Time on a recording is seconds since the recording started, matching the live
axis; the wall clock a recording has appears in the cursor readout. The
recorder's `log_time` is what is plotted, not the publisher's `publish_time`.

Binding a signal on a recording starts one pass over the whole file on a
background thread, so a trace is empty until its decode finishes, the same
state a live signal is in before its publisher says anything. `scope.source`
reports `decodes_pending`; anything read or screenshotted before it is zero is
an unfinished picture, not a broken one. A message recorded under a different
schema than the binding expects is skipped, not decoded.

## Agent control

Everything in [agent control](../developing/agent-control.html) applies; scope
registers `ui.*`, `input.*`, `app.*` and `zenoh.*` like the others, plus:

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
| `scope.stats` | what each panel has received, whatever kind it is |
| `scope.describe_stats` | what fields `scope.stats` will return for that panel |
| `scope.save` / `scope.load` | workspaces |
| `scope.settings` | the per-user tileset list: read with no params, replace with `tilesets` |
| `scope.source` | `mode` (online/offline), `kind` (live/recorded/empty), and `decodes_pending` |
| `scope.set_mode` | `{"mode": "online"}` attaches to the bus and starts capturing; `"offline"` detaches and lands on the capture |
| `scope.open_recording` | open a bag directory; implies offline |
| `scope.capture` | messages, bytes, retained span, `evicted`; `running: false` once offline |
| `scope.review_capture` / `scope.save_recording` | review the session capture; write it out as a bag |
| `scope.sample_stats` | the time-series half of `scope.stats`, under its historical name |

The loop that closes fastest:

```
app_launch(app="scope")
scope_source(online=True)     # FIRST. A window starts offline and sees no topics.
scope_add_panel(type="time_series", id="plot1")
scope_browser(rescan=true)
scope_add_signal(panel="plot1", zenoh_key="vehicle/engine/rpm", field="rpm")
scope_sample_stats()          # count, drops, min/max: is the data there at all?
ui_screenshot()               # and then actually look
```

`scope.sample_stats` is the one to reach for first. A screenshot shows a line;
this says what the line is made of. `received` climbing with `dropped` at zero
and a `min`/`max` bracketing what you published is a stronger statement than a
picture. The map panel, end to end:

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

`scope.time_base` refuses two movers in one call: `{"pan": -5, "zoom": 0.5}`
is `BAD_PARAMS`, because the result could not be read back from the request.
Every `scope.*` method sees settled buffers, so "set the view, then read
`sample_stats`" observes exactly the buffers it asked for. Writing
`scope.settings` replaces the whole tileset list rather than merging, and the
Settings dialog refuses to open under `--mcp`, where a modal has nobody to
dismiss it. The recipes that prove a seek, a pan, a zoom anchor, the density
histogram and a video seek without a screenshot are under
[verifying it headlessly](../design/scope-internals.html#verifying-it-headlessly).

{: .note }
An `input_click` on a panel has to target the panel, not its dock. The dock
carries the panel's `objectName`, so `target="map1"` resolves to the dock and
a click lands on its title bar. `ui_snapshot` gives the panel's path.

## Troubleshooting

**The signal browser is empty.** Scope is offline, which is the default, and
has opened no zenoh session. Press `Offline` on the toolbar, start with
`--online`, or load a recording.

**A topic is listed but its trace is flat and empty.** The picker shows every
topic whose node is up, not every topic with traffic. Check
`scope.sample_stats`: `received` at zero means nothing is arriving. If you
bound a list element such as `values[7]`, a message whose list is shorter than
the schema's declared length is skipped and reported once.

**Every table cell reads `--` after pausing.** The readout instant is frozen
in the past while the view is paused, and a signal bound after that has no
sample at or before it. Resume, or set `follow_cursor: false`. A row that has
a value but is dimmed is stale: its publisher stopped, and the age column says
how long ago.

**The video panel is black.** Read `scope.stats` for the panel; the Video
section above says which counter names which fault. On a recording, wait for
`decodes_pending` to reach zero.

**The map is empty.** The panel captions itself and `scope.stats` repeats the
caption in `diagnostic`; the table under Map lists every message.

**A seek landed somewhere other than where I asked.** The window slides into
the available range rather than being squashed, so with the default 30 s
window a seek to `20` shows `[0, 30]`. Narrow the window first.

**`--mcp /tmp/a.sock` exits with "Unrecognised argument".** The socket path
goes after `=`.

**A headless run hangs with no log line.** A modal dialog was raised with
nobody to dismiss it. Under `--mcp`, call the dialog-free methods
(`scope.load`, `scope.save`, `scope.open_recording`, `scope.save_recording`,
`scope.settings`); the unsaved-changes prompt returns true with a warning.

**A workspace loaded with a panel missing, or the layout defaulted.** The log
names the panel: an unknown `type:` is skipped with a warning, and a panel
with no `id:` loses its saved position. A layout that did not restore has a
`dock_state` from another Qt version; the panels are all there.

**Settings changed by themselves after a test run.** The test ran without
`--settings` and wrote to your real file. Pass `--settings <path>` to every
`ctest` and `--mcp` run.

**Quitting asks about the capture before the workspace.** Deliberate: a
workspace can be rebuilt by hand in a couple of minutes, and a capture of what
the vehicle was doing cannot be rebuilt at all.
