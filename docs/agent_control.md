# Agent control interface (`--mcp`)

The `dashboard` and `editor` both ship with a control endpoint that lets an agent
run them headless, look at them, and drive them. It is always compiled in and
only listens when `--mcp` is supplied.

```
Claude Code
    |  MCP (stdio)
    v
tools/mcp_dashboard/            Python, uv. Lifecycle + tool schemas + PNG encoding.
    |  newline-delimited JSON-RPC 2.0 over an AF_UNIX socket
    +--> dashboard --mcp=/path/a.sock -c configs/dashboard/...
    +--> editor    --mcp=/path/b.sock [-c ...]
```

The C++ side (`libs/agent_control`) deliberately does not speak MCP. It exposes
methods; tool names, descriptions and argument schemas live in Python where they
change without a rebuild.

> The library is `agent_control`, not `mcp_*`: `libs/mcp2221a` is the Microchip
> USB-to-I2C bridge driver and the collision would be confusing. The user-facing
> flag is still `--mcp`.

## Running it by hand

```bash
cmake --build build --target dashboard editor
./build/dashboard/dashboard --mcp=/tmp/a.sock -c configs/dashboard/mercedes_190e_dash.yaml
# prints: AGENT_READY /tmp/a.sock <pid>   (and no window: the platform is forced to offscreen)

printf '{"jsonrpc":"2.0","id":1,"method":"ui.snapshot","params":{}}\n' | nc -U /tmp/a.sock
```

**`--mcp` takes its value with `=`.** `--mcp /tmp/a.sock` does *not* bind the
path — cxxopts options with an implicit value do not consume a space-separated
argument — so the apps refuse to start rather than listening somewhere other than
where you asked. Bare `--mcp` uses `/tmp/redline_agent_<pid>.sock`.

`--mcp` also replaces the rotating file sink with a stderr sink, keeping stdout
clean for the `AGENT_READY` handshake. The companion server captures that stderr,
so a crash comes back with the log tail that preceded it.

## Under Claude Code

`.mcp.json` in the repo root registers the server, so a fresh clone gets it with
no setup beyond having `uv` installed:

```json
{ "mcpServers": { "redline": {
    "command": "uv",
    "args": ["run", "--directory", "tools/mcp_dashboard", "redline-mcp"] } } }
```

It lives in the repo on purpose. The server's tool descriptions encode the
selector grammar, the coordinate contract and the widget types — all of which
change with the code they describe, so a copy kept anywhere else drifts silently.
`uv.lock` is committed alongside it, so dependency versions are pinned rather
than resolved fresh on each machine.

Two assumptions worth knowing:

- **The `--directory` path is relative**, so the client must launch the server
  with the repo root as its working directory. Claude Code does. Another client
  that does not would fail at `uv` before Python starts — use an absolute path in
  that client's own config if so.
- **The server locates the repo by walking up for `.git` + `CMakeLists.txt`**,
  not by counting directories, so it works regardless of where it is launched
  from once running. `REDLINE_REPO_ROOT` overrides it; `REDLINE_BUILD_DIR`
  overrides where it looks for binaries.

Then:

```
app_launch(app="dashboard", config="configs/dashboard/mercedes_190e_dash.yaml")
ui_snapshot(interactive_only=True)     # what exists, and what to call it
ui_screenshot(target="#speedo")        # look at it
input_click(target="#speedo", x=125, y=125)
```

**At most one `dashboard` and one `editor` at a time.** Running two dashboards is
not supported: they share a zenoh bus, so a second would observe samples injected
at the first. `app_launch` on a type that is already running replaces it.

## Addressing widgets

Selectors, in the order they are tried:

| Form | Example | Notes |
|---|---|---|
| ref | `w7` | From a previous `ui.snapshot`. Stable while the widget lives. |
| id | `#speedo` | The optional `id:` key in the widget's YAML entry. |
| derived name | `mercedes_190e_speedometer#0` | Automatic fallback; also just an objectName. Shifts when widgets are reordered — set an `id:` for anything you address repeatedly. |
| path | `MainWindow/Mercedes190ETelltale[1]` | Class names, `[n]` for identical siblings. Each segment is a strict parent→child step. |
| bare token | `CarPlayWidget` | Class or id, anywhere in the tree. |

A selector matching **nothing** or **more than one** widget is always an error,
and the error lists the candidates. Never a silent first match: driving the wrong
widget produces a confident wrong conclusion, which is worse than failing.

Both apps name widgets by the same rule (`dashboard/include/dashboard/widget_identity.h`),
so one selector addresses the same widget in the dashboard and in the editor.

## The coordinate contract

> **Every coordinate is a logical pixel, local to the widget you name. There are
> no screen coordinates anywhere in this interface.**

`ui.screenshot` returns `scale`, `logical_rect`, `image_size` and `dpr` with the
image, so an image pixel converts to a click position with
`x = px/scale + logical_rect[0]`.

This is why the **CarPlay** widget needs no special handling. It renders the
phone's video into a `QImage` and normalises `pos.x()/width()` itself before
publishing a touch, so a coordinate read off its screenshot is already the right
thing to send to `input.click`.

## The debugging loop that matters most

The dashboard is a data-driven display, so most questions are answered by setting
a value and looking at the gauge:

```
zenoh_describe_schema(schema="VehicleSpeed")     # what fields exist
zenoh_publish("vehicle/speed_mps", "VehicleSpeed", {"speedMps": 27.0})
ui_screenshot(target="#speedo")                  # needle sits at 60 mph
```

No `test_data_publisher`, no CAN bus, no car. It works for every schema in the
registry with no per-schema code, because it goes through Cap'n Proto's dynamic
API — a schema added to `schemas/CMakeLists.txt` is publishable immediately.

Two zenoh properties that will otherwise cost you time, and which the error
messages now state outright:

- **Discovery only sees live traffic.** `zenoh_list` subscribes for a window and
  reports what arrives. An empty result means "nothing published during the
  window", not "nothing exists" — a slow publisher looks identical to an absent
  one.
- **There are no retained messages.** Reading back a value you published once
  will *always* time out, because the read subscribes after that sample is gone.
  Read from a continuous publisher instead. (This is the same property behind the
  CarPlay video-config black screen.)

## Gotchas worth knowing

- **Screenshots work only because rendering goes through Qt's backing store.**
  `QWidget::grab()` renders the backing store, so a `QVideoWidget`,
  `QOpenGLWidget` or `QRhiWidget` captures as black. The CarPlay widget was
  reverted from `QVideoWidget` to a `QImage` blit (commit `4d143ae`) for
  z-ordering, and that revert is the only reason video screenshots work.
  `WA_NativeWindow` reads FALSE on those widgets so it cannot be used to detect
  the problem; `capture.cpp` class-checks the subtree and puts a `warning` in the
  screenshot metadata instead of returning a plausible black image.
- **`GUI_THREAD_BUSY` is an answer, not a bug.** Handlers are posted to the GUI
  thread with a timeout rather than a blocking connection, precisely so that a
  wedged UI can still be diagnosed instead of hanging the caller too.
- **`mouse_transparent` in a snapshot** explains a click that lands correctly and
  does nothing: `Canvas::setEditorMode()` applies `WA_TransparentForMouseEvents`
  recursively when editor mode is off.
- **`accepted: false` from `input.click`** means the event was delivered but
  nothing consumed it. Normal for a decorative gauge; a real clue for a widget
  you expected to react.
- **A single screenshot proves very little on an animated widget.** The CarPlay
  simulator's test pattern moves, so two frames legitimately look wildly
  different and a colour "regression" can be pure frame timing. Compare hashes
  across several frames, or check pixel statistics, before concluding anything
  about rendering.
- **Editor edits are undoable, including the ones you make.** `editor.add_widget`,
  `editor.delete`, `editor.move`, `editor.resize` and `widget.set_config` all go
  onto the same history the GUI's Ctrl+Z uses, so `editor.undo` / `editor.redo`
  will walk back a sequence an agent built. Both report `can_undo`, `can_redo`
  and `dirty`, so you can tell whether the layout still differs from the file on
  disk without saving it to find out. Widget names survive an undo — a selector
  you are holding stays valid.
- **The editor does not prompt about unsaved work under `--mcp`.** A modal
  dialog with nobody at the screen is a hang, not a question, so `app_launch`
  replacing an editor discards unsaved changes with a log line and no dialog.
  `editor.save` first if you care about them.
- **Use `carplay --simulate` for anything CarPlay-shaped.** It publishes a
  synthetic session — H.264 video, PCM audio, rotating metadata — on the real
  zenoh topics, so the whole dashboard side is exercisable with no iPhone
  attached. Launch it through `app_launch`'s sibling process handling or by hand.

## Adding a method

`AgentServer::registerMethod(name, handler, kind)`. Handlers always run on the
GUI thread and may touch widgets directly. Mark a handler `kMutating` if it
changes state — the dispatcher then drains the event loop before returning, so a
following screenshot observes the effect rather than the previous frame. See the
`editor.save` / `editor.load` registrations in `dashboard/editor/main.cpp` for the
shape.

New methods are reachable from Claude Code immediately via `app_call(method,
params)` — no Python change needed. `app_methods` lists what a running build
supports.

## Dragging, and the one thing that cannot be synthesized

`input.drag` sends press, interpolated moves and release. It makes sure the first
move clears `QApplication::startDragDistance()` whenever the overall distance
does — Qt reads shorter motion as jitter, so a widget filtering on it would
otherwise ignore the whole gesture. A drag too short to register at all comes
back with a warning rather than appearing to work. Use it for CarPlay swipes and
for moving or resizing on the editor canvas.

**It does not drive Qt's `QDrag`.** `QDrag::exec()` runs a nested event loop that
grabs the mouse and reads real platform events; synthesized events cannot advance
it, and on the offscreen platform it may not run at all. So the palette-to-canvas
drag goes through `input.drop` / `editor.palette_drag`, which send the
`QDragEnter` → `QDragMove` → `QDrop` triple straight to the drop target. That
bypasses the drag *source* but runs the entire receiving side — the accept/reject
logic and the drop handler — which is where the behaviour worth testing lives.
The only thing left uncovered is the handful of lines inside `exec()`.

## Logs

`--mcp` replaces the rotating file sink with an in-process ring plus stderr.
`app.logs` takes a `since_seq` cursor, so polling returns only what is new:

```
app_logs(limit=20)                  # -> records + next_seq
app_logs(since_seq=<next_seq>)      # -> only what arrived since
app_logs(level="warn", grep="carplay")
```

Qt's own diagnostics are bridged into the same stream under the logger `qt`.
Nothing captured them before — they went to stderr and vanished — and a QPA or
layout complaint is often the explanation for a screenshot that came back wrong.

A `dropped` field means the ring wrapped and records were lost before you read
them. The stderr sink stays because the ring dies with the process: a crash would
otherwise take the entire log history with it, and the companion server captures
that stderr for exactly that case.

## Tests

```bash
ctest --test-dir build -R agent_control --output-on-failure
```

- `agent_control_test_framing` (unit) — the JSON-RPC envelope, weighted towards
  malformed input: truncated JSON, wrong-typed fields, throwing handlers, and the
  invariant that a response never contains a raw newline.
- `agent_control_test_selector` (gui) — selector resolution against a real widget
  tree, weighted towards ambiguity, staleness and out-of-range indices.
- `agent_control_test_log_ring` (unit) — the cursor, filtering, eviction
  reporting and the Qt message bridge. This one caught a real off-by-one: an
  exclusive lower bound against a field named `next_seq` silently dropped one
  record per poll.

## Status

Everything in the plan is implemented:

| Area | Methods |
|---|---|
| App | `app.info`, `app.logs`, `app.quit` |
| Inspect | `ui.snapshot`, `ui.find`, `ui.screenshot` (with `annotate`, `if_changed_from`), `ui.wait_for` |
| Input | `input.click`, `input.key`, `input.type`, `input.drag`, `input.drop` |
| Widget config | `widget.describe_config`, `widget.get_config`, `widget.set_config` |
| Zenoh | `zenoh.list`, `zenoh.read`, `zenoh.publish`, `zenoh.rate`, `zenoh.describe_schema` |
| Editor | `editor.palette`, `editor.items`, `editor.add_widget`, `editor.palette_drag`, `editor.select`, `editor.move`, `editor.resize`, `editor.delete`, `editor.set_mode`, `editor.undo`, `editor.redo`, `editor.save`, `editor.load` |
| Meta | `rpc.methods` |

Known limits, all deliberate: one instance per app type; `QDrag::exec()` is not
driven; `Data` fields are reported as a byte count rather than inlined (they are
H.264 access units and PCM audio); and screenshots depend on rendering going
through Qt's backing store.
