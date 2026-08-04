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

`.mcp.json` in the repo root registers the server. Then:

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

## Tests

```bash
ctest --test-dir build -R agent_control --output-on-failure
```

- `agent_control_test_framing` (unit) — the JSON-RPC envelope, weighted towards
  malformed input: truncated JSON, wrong-typed fields, throwing handlers, and the
  invariant that a response never contains a raw newline.
- `agent_control_test_selector` (gui) — selector resolution against a real widget
  tree, weighted towards ambiguity, staleness and out-of-range indices.

## Status

Implemented: `app.info`, `app.quit`, `ui.snapshot`, `ui.find`, `ui.screenshot`,
`input.click`, `input.key`, `input.type`, `rpc.methods`, plus `editor.save` and
`editor.load`.

Not yet implemented (see the plan for the full design): the structured log ring
and `app.logs`, `widget.get/set/describe_config`, `ui.wait_for`, `input.drag`,
`input.drop` and the rest of the editor verbs, and the zenoh
publish/read/list/rate methods.
