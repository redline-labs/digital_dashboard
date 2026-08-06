"""MCP server exposing the dashboard and editor control interface."""

from __future__ import annotations

import base64
import json
from typing import Annotated, Any, Literal

from mcp.server.mcpserver import Image, MCPServer
from pydantic import Field

from .client import AgentError
from .supervisor import AppName, LaunchError, Supervisor

# Repeated verbatim on every tool that takes an `app`. The type is already a
# closed enum, which makes a second instance of any type unrepresentable in the
# schema; this says why, so the constraint reads as intentional rather than as
# a gap.
APP_FIELD = Field(
    default=None,
    description=(
        "Which app to target: 'dashboard', 'editor' or 'scope'. Optional when only "
        "one is running. There is at most one instance of each type."
    ),
)

TARGET_FIELD = Field(
    description=(
        "Widget selector. One of: '#my_id' (the YAML `id:`), "
        "'mercedes_190e_speedometer#0' (derived name), "
        "'MainWindow/CarPlayWidget[0]' (tree path), or 'w7' (a ref from "
        "ui_snapshot). Matching nothing, or more than one widget, is an error "
        "that lists the candidates."
    )
)

INSTRUCTIONS = """\
Drives the Redline dashboard, editor and scope headlessly (Qt 'offscreen'
platform) so you can see and interact with them without a human at the screen.

AT MOST ONE OF EACH APP TYPE RUNS AT A TIME. There is no support for running two
dashboards: instances share a zenoh bus, so a second one would observe samples
injected at the first. `app_launch` on a type that is already running REPLACES
that instance (any unsaved editor or scope state is lost). To run several
together, call `app_launch` once per type.

COORDINATES: every coordinate in this interface is in logical pixels, local to
the widget you name. There are no screen coordinates anywhere. `ui_screenshot`
returns `scale` and `logical_rect` alongside the image, so a pixel (px, py) you
picked off a screenshot converts to a click position with
`x = px/scale + logical_rect[0]`, `y = py/scale + logical_rect[1]`.

This is what makes the CarPlay widget work like anything else: it renders the
phone's video into a QImage and normalises positions itself, so a coordinate read
off its screenshot is already the right thing to send to input_click.

Typical loop: app_launch -> ui_snapshot (to see what exists and get ids) ->
ui_screenshot (to see it) -> input_click / input_key -> ui_screenshot again.
When something looks wrong, app_logs is usually faster than guessing.
"""

mcp = MCPServer("redline", instructions=INSTRUCTIONS)
supervisor = Supervisor()


def _fail(exc: Exception) -> str:
    """Render an error as something actionable rather than a bare traceback."""
    if isinstance(exc, AgentError):
        detail = {k: v for k, v in exc.data.items() if k != "reason"}
        text = f"{exc.reason}: {exc}"
        if detail:
            text += "\n" + json.dumps(detail, indent=2)
        return text
    return f"{type(exc).__name__}: {exc}"


def _call(app: AppName | None, method: str, params: dict[str, Any] | None = None) -> Any:
    instance = supervisor.get(app)
    return instance.client.call(method, params)


def _screenshot_content(result: dict[str, Any]) -> list[Any]:
    """Split a capture result into an image plus its coordinate metadata.

    The metadata always travels with the image -- an image whose scale is unknown
    cannot be turned back into a click position, which would defeat the point.
    """
    meta = {k: v for k, v in result.items() if k != "image_png_base64"}
    parts: list[Any] = [json.dumps(meta, indent=2)]
    encoded = result.get("image_png_base64")
    if encoded:
        # to_image_content(), not the Image itself: the server only serializes
        # content types when they are returned inside a list, and a bare Image
        # comes back as "Unable to serialize unknown type" -- which fails every
        # screenshot, i.e. the one tool you cannot work around by guessing.
        parts.append(Image(data=base64.b64decode(encoded), format="png").to_image_content())
    return parts


# --------------------------------------------------------------------- lifecycle


@mcp.tool()
def app_launch(
    app: Annotated[AppName, Field(description="Which app to launch.")],
    config: Annotated[
        str | None,
        Field(
            default=None,
            description=(
                "Path to a YAML config, relative to the repo root, e.g. "
                "'configs/dashboard/mercedes_190e_dash.yaml'. Required for the "
                "dashboard; optional for the editor (it opens empty without one)."
            ),
        ),
    ] = None,
    extra_args: Annotated[
        list[str] | None,
        Field(default=None, description="Additional command-line arguments."),
    ] = None,
) -> str:
    """Launch the dashboard or editor headless, with the control interface enabled.

    ONE INSTANCE PER TYPE. If this app type is already running, it is QUIT AND
    REPLACED -- any unsaved editor state is lost. Running two dashboards is not
    supported. To have a dashboard and an editor up at the same time, call this
    once for each.

    Waits for the app to report readiness before returning, so the next call can
    address widgets immediately. If the app crashes on startup, the failure comes
    back with its stderr rather than as a timeout.
    """
    try:
        instance = supervisor.launch(app, config, extra_args)
    except (LaunchError, OSError) as exc:
        return _fail(exc)

    return json.dumps(
        {
            "app": instance.app,
            "pid": instance.process.pid,
            "config_path": instance.config_path,
            "socket_path": instance.socket_path,
        },
        indent=2,
    )


@mcp.tool()
def app_list() -> str:
    """List the running apps. At most two rows: one dashboard and one editor."""
    return json.dumps(supervisor.list(), indent=2)


@mcp.tool()
def app_quit(app: Annotated[AppName, Field(description="Which app to quit.")]) -> str:
    """Stop an application and clean up its process."""
    return "stopped" if supervisor.quit(app) else f"no {app} was running"


@mcp.tool()
def app_restart(
    app: Annotated[AppName, Field(description="Which app to restart.")],
) -> str:
    """Relaunch an app with the same config it was started with.

    Use after rebuilding, to pick up the new binary.
    """
    try:
        current = supervisor.get(app)
        config = current.config_path
    except LaunchError as exc:
        return _fail(exc)

    try:
        instance = supervisor.launch(app, config)
    except (LaunchError, OSError) as exc:
        return _fail(exc)
    return json.dumps({"app": instance.app, "pid": instance.process.pid}, indent=2)


@mcp.tool()
def app_info(app: Annotated[AppName | None, APP_FIELD] = None) -> str:
    """Report app identity, config path, Qt platform, and top-level windows."""
    try:
        return json.dumps(_call(app, "app.info"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def app_logs(
    app: Annotated[AppName | None, APP_FIELD] = None,
    since_seq: Annotated[
        int,
        Field(
            default=0,
            description=(
                "Return records from this sequence number onwards. Feed back the "
                "`next_seq` from your previous call to get only what is new. "
                "0 means from the oldest record still held."
            ),
        ),
    ] = 0,
    level: Annotated[
        str | None,
        Field(
            default=None,
            description="Minimum severity: trace, debug, info, warn, err, critical.",
        ),
    ] = None,
    grep: Annotated[
        str | None,
        Field(default=None, description="Case-insensitive substring of the message."),
    ] = None,
    logger: Annotated[
        str | None,
        Field(default=None, description="Exact logger name. Qt's own messages use 'qt'."),
    ] = None,
    limit: Annotated[
        int, Field(default=200, description="Maximum records; keeps the newest.")
    ] = 200,
) -> str:
    """Read the app's in-process log ring: structured, filterable, with a cursor.

    This is usually faster than guessing when something looks wrong. The app logs
    each stage of its work, and Qt's own diagnostics (QPA errors, layout
    warnings) are bridged into the same stream under the 'qt' logger -- a
    screenshot that came back wrong often has a Qt line behind it.

    A `dropped` field means the ring wrapped and records were lost before you
    read them; poll more often if you see it.
    """
    try:
        return json.dumps(
            _call(
                app,
                "app.logs",
                {
                    "since_seq": since_seq,
                    "level": level,
                    "grep": grep,
                    "logger": logger,
                    "limit": limit,
                },
            ),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


# ---------------------------------------------------------------------- inspect


@mcp.tool()
def ui_snapshot(
    app: Annotated[AppName | None, APP_FIELD] = None,
    target: Annotated[
        str | None,
        Field(default=None, description="Root to snapshot from. Defaults to all windows."),
    ] = None,
    depth: Annotated[
        int, Field(default=-1, description="Maximum depth; -1 for unlimited.")
    ] = -1,
    interactive_only: Annotated[
        bool,
        Field(
            default=False,
            description=(
                "Drop pure scaffolding (layout containers, Qt internals). Keeps "
                "anything with an explicit id and all custom widgets. Start here: "
                "it is far cheaper to read."
            ),
        ),
    ] = False,
    include_invisible: Annotated[
        bool, Field(default=False, description="Include hidden widgets.")
    ] = False,
) -> str:
    """List the addressable widgets: id, class, path, geometry, visibility, ref.

    This is the map of what you can click and screenshot. `rect` is widget-local
    (the space input_click takes); `window_rect` is where the widget sits in its
    window. A `mouse_transparent` flag means the widget does not normally receive
    clicks -- the usual reason a click appears to do nothing.
    """
    try:
        return json.dumps(
            _call(
                app,
                "ui.snapshot",
                {
                    "target": target,
                    "depth": depth,
                    "interactive_only": interactive_only,
                    "include_invisible": include_invisible,
                },
            ),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def ui_find(
    query: Annotated[str, Field(description="Substring matched against id and class name.")],
    app: Annotated[AppName | None, APP_FIELD] = None,
) -> str:
    """Search widgets by id or class name, case-insensitively."""
    try:
        return json.dumps(_call(app, "ui.find", {"query": query}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def ui_screenshot(
    app: Annotated[AppName | None, APP_FIELD] = None,
    target: Annotated[
        str | None,
        Field(default=None, description="Widget to capture. Defaults to the main window."),
    ] = None,
    region: Annotated[
        list[int] | None,
        Field(
            default=None,
            description="Widget-local crop as [x, y, width, height]. Defaults to the whole widget.",
        ),
    ] = None,
    max_dim: Annotated[
        int,
        Field(default=1024, description="Longest edge of the returned image, in pixels."),
    ] = 1024,
    if_changed_from: Annotated[
        str | None,
        Field(
            default=None,
            description=(
                "A `hash` from an earlier screenshot. If the view is unchanged, no "
                "image is returned. Use this when polling for an effect -- it makes "
                "the no-change case nearly free."
            ),
        ),
    ] = None,
    annotate: Annotated[
        bool,
        Field(
            default=False,
            description=(
                "Overlay numbered boxes on the addressable widgets and return a "
                "mark->selector map. Use this when several widgets look alike and "
                "you need to be sure which one you are aiming at."
            ),
        ),
    ] = False,
) -> list[Any]:
    """Capture a widget as a PNG, with the metadata needed to click on it.

    Returns `scale`, `logical_rect`, `image_size` and `dpr` alongside the image.
    Convert an image pixel to a click position with x = px/scale + logical_rect[0].

    If the result carries a `warning` about non-backing-store widgets, the image
    is NOT a faithful view: something in that subtree composites outside Qt's
    backing store and captures as black.
    """
    try:
        result = _call(
            app,
            "ui.screenshot",
            {
                "target": target,
                "region": region,
                "max_dim": max_dim,
                "if_changed_from": if_changed_from,
                "annotate": annotate,
            },
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]
    return _screenshot_content(result)


# ------------------------------------------------------------------------ input


@mcp.tool()
def input_click(
    target: Annotated[str, TARGET_FIELD],
    app: Annotated[AppName | None, APP_FIELD] = None,
    x: Annotated[
        float | None,
        Field(default=None, description="Widget-local x. Omit (with y) to click the centre."),
    ] = None,
    y: Annotated[float | None, Field(default=None, description="Widget-local y.")] = None,
    button: Annotated[
        Literal["left", "right", "middle"], Field(default="left", description="Mouse button.")
    ] = "left",
    modifiers: Annotated[
        str | None,
        Field(default=None, description="Held modifiers, '+'-joined, e.g. 'ctrl+shift'."),
    ] = None,
    count: Annotated[int, Field(default=1, description="1 for a click, 2 for a double click.")] = 1,
    screenshot: Annotated[
        bool,
        Field(default=False, description="Also return a screenshot of the window afterwards."),
    ] = False,
) -> list[Any]:
    """Click a widget, in widget-local logical pixels.

    Coordinates are NOT screen coordinates. Take a ui_screenshot of the same
    target, then convert: x = px/scale + logical_rect[0].

    The reply's `accepted` says whether the widget handled the press. False means
    the click was delivered but nothing consumed it -- normal for a decorative
    widget, a real clue for one you expected to react.
    """
    try:
        result = _call(
            app,
            "input.click",
            {
                "target": target,
                "x": x,
                "y": y,
                "button": button,
                "modifiers": modifiers,
                "count": count,
                "screenshot": screenshot,
            },
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    shot = result.pop("screenshot", None)
    parts: list[Any] = [json.dumps(result, indent=2)]
    if shot:
        parts.extend(_screenshot_content(shot))
    return parts


@mcp.tool()
def input_key(
    keys: Annotated[
        str, Field(description="Key sequence in portable form: 'Ctrl+S', 'Delete', 'F5'.")
    ],
    app: Annotated[AppName | None, APP_FIELD] = None,
    target: Annotated[
        str | None,
        Field(default=None, description="Widget to send to. Defaults to the focus widget."),
    ] = None,
    screenshot: Annotated[
        bool, Field(default=False, description="Also return a screenshot afterwards.")
    ] = False,
) -> list[Any]:
    """Send a key sequence to a widget (or to whatever has focus)."""
    try:
        result = _call(
            app, "input.key", {"target": target, "keys": keys, "screenshot": screenshot}
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    shot = result.pop("screenshot", None)
    parts: list[Any] = [json.dumps(result, indent=2)]
    if shot:
        parts.extend(_screenshot_content(shot))
    return parts


@mcp.tool()
def input_type(
    text: Annotated[str, Field(description="Literal text to type.")],
    app: Annotated[AppName | None, APP_FIELD] = None,
    target: Annotated[
        str | None,
        Field(default=None, description="Widget to type into. Defaults to the focus widget."),
    ] = None,
    screenshot: Annotated[
        bool, Field(default=False, description="Also return a screenshot afterwards.")
    ] = False,
) -> list[Any]:
    """Type literal text into a widget, one key event per character."""
    try:
        result = _call(
            app, "input.type", {"target": target, "text": text, "screenshot": screenshot}
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    shot = result.pop("screenshot", None)
    parts: list[Any] = [json.dumps(result, indent=2)]
    if shot:
        parts.extend(_screenshot_content(shot))
    return parts


@mcp.tool()
def ui_wait_for(
    target: Annotated[str, TARGET_FIELD],
    app: Annotated[AppName | None, APP_FIELD] = None,
    condition: Annotated[
        Literal["exists", "visible", "gone", "enabled"],
        Field(default="visible", description="What to wait for."),
    ] = "visible",
    timeout_ms: Annotated[
        int, Field(default=3000, description="How long to wait before giving up.")
    ] = 3000,
) -> str:
    """Wait until a widget reaches a state, pumping the app's event loop.

    Use this instead of taking a screenshot and hoping: it is the difference
    between a check that is deterministic and one that is a race. The reply says
    whether the condition was met and how long it took.
    """
    try:
        return json.dumps(
            _call(
                app,
                "ui.wait_for",
                {
                    "target": target,
                    "condition": condition,
                    "timeout_ms": timeout_ms,
                    # The dispatcher's own deadline has to outlast the wait, or it
                    # gives up first and reports a busy GUI thread instead of a
                    # plain timeout.
                    "_timeout_ms": timeout_ms + 5000,
                },
            ),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def input_drag(
    target: Annotated[str, TARGET_FIELD],
    from_x: Annotated[float, Field(description="Start x, widget-local.")],
    from_y: Annotated[float, Field(description="Start y, widget-local.")],
    to_x: Annotated[float, Field(description="End x, widget-local.")],
    to_y: Annotated[float, Field(description="End y, widget-local.")],
    app: Annotated[AppName | None, APP_FIELD] = None,
    steps: Annotated[
        int, Field(default=10, description="Intermediate move events between press and release.")
    ] = 10,
    hold_ms: Annotated[
        int, Field(default=0, description="Pause after pressing, before moving.")
    ] = 0,
    screenshot: Annotated[
        bool, Field(default=False, description="Also return a screenshot afterwards.")
    ] = False,
) -> list[Any]:
    """Press, move and release: a mouse drag in widget-local coordinates.

    This is the right tool for a CarPlay swipe and for moving or resizing a
    widget on the editor canvas. It is NOT the tool for dragging from the editor
    palette onto the canvas -- that uses Qt's QDrag, whose nested event loop
    reads real platform events and cannot be driven synthetically. Use
    editor_palette_drag for that.

    A drag shorter than Qt's startDragDistance comes back with a warning, since
    Qt may read it as a click.
    """
    try:
        result = _call(
            app,
            "input.drag",
            {
                "target": target,
                "from_x": from_x,
                "from_y": from_y,
                "to_x": to_x,
                "to_y": to_y,
                "steps": steps,
                "hold_ms": hold_ms,
                "screenshot": screenshot,
            },
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    shot = result.pop("screenshot", None)
    parts: list[Any] = [json.dumps(result, indent=2)]
    if shot:
        parts.extend(_screenshot_content(shot))
    return parts


@mcp.tool()
def input_drop(
    target: Annotated[str, TARGET_FIELD],
    x: Annotated[float, Field(description="Drop x, widget-local.")],
    y: Annotated[float, Field(description="Drop y, widget-local.")],
    mime: Annotated[
        dict[str, str],
        Field(description='Mime payload, e.g. {"text/plain": "carplay"}.'),
    ],
    app: Annotated[AppName | None, APP_FIELD] = None,
    screenshot: Annotated[
        bool, Field(default=False, description="Also return a screenshot afterwards.")
    ] = False,
) -> list[Any]:
    """Synthesize a drag-and-drop onto a widget that accepts drops.

    Sends the real QDragEnter/QDragMove/QDrop sequence straight to the target,
    bypassing the drag source (QDrag::exec cannot be driven synthetically). The
    receiving side -- the accept/reject logic and the drop handler -- runs for
    real, which is where the behaviour worth testing lives.

    If the widget rejects the drag on entry you get an error naming the formats
    you sent, which usually means they do not match what its handler looks for.
    """
    try:
        result = _call(
            app,
            "input.drop",
            {"target": target, "x": x, "y": y, "mime": mime, "screenshot": screenshot},
        )
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    shot = result.pop("screenshot", None)
    parts: list[Any] = [json.dumps(result, indent=2)]
    if shot:
        parts.extend(_screenshot_content(shot))
    return parts


# --------------------------------------------------------------------- editor


@mcp.tool()
def editor_palette() -> str:
    """List the widget types that can be added, with their friendly names."""
    try:
        return json.dumps(_call("editor", "editor.palette"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_items() -> str:
    """List what is currently on the editor canvas, with ids and geometry."""
    try:
        return json.dumps(_call("editor", "editor.items"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_add_widget(
    type: Annotated[str, Field(description="Widget type, e.g. 'carplay'. See editor_palette.")],
    x: Annotated[int, Field(description="Canvas x.")],
    y: Annotated[int, Field(description="Canvas y.")],
    width: Annotated[
        int | None,
        Field(default=None, description="Width; omit (with height) to use the size hint."),
    ] = None,
    height: Annotated[int | None, Field(default=None, description="Height.")] = None,
) -> str:
    """Add a widget to the editor canvas and select it.

    Goes through the same Canvas::addWidget that a palette drag ends up calling,
    so the result is identical to dragging one in by hand.
    """
    try:
        return json.dumps(
            _call(
                "editor",
                "editor.add_widget",
                {"type": type, "x": x, "y": y, "width": width, "height": height},
            ),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_palette_drag(
    type: Annotated[str, Field(description="Widget type to drag in.")],
    x: Annotated[float, Field(description="Canvas x to drop at.")],
    y: Annotated[float, Field(description="Canvas y to drop at.")],
) -> str:
    """Drag a widget from the palette onto the canvas, the way a user would.

    Builds the mime payload exactly as WidgetPalette::startDrag does and feeds it
    to the real drop path. Prefer this over editor_add_widget when you want to
    exercise the drag-and-drop code rather than just arrange a layout: it covers
    everything except the few lines inside QDrag::exec().
    """
    try:
        return json.dumps(
            _call("editor", "editor.palette_drag", {"type": type, "x": x, "y": y}), indent=2
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_move(
    target: Annotated[str, TARGET_FIELD],
    x: Annotated[int, Field(description="New canvas x.")],
    y: Annotated[int, Field(description="New canvas y.")],
) -> str:
    """Move a widget on the editor canvas to an exact position."""
    try:
        return json.dumps(_call("editor", "editor.move", {"target": target, "x": x, "y": y}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_resize(
    target: Annotated[str, TARGET_FIELD],
    width: Annotated[int, Field(description="New width.")],
    height: Annotated[int, Field(description="New height.")],
) -> str:
    """Resize a widget on the editor canvas."""
    try:
        return json.dumps(
            _call("editor", "editor.resize", {"target": target, "width": width, "height": height}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_select(target: Annotated[str, TARGET_FIELD]) -> str:
    """Select a widget on the canvas, as clicking it would."""
    try:
        return json.dumps(_call("editor", "editor.select", {"target": target}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_delete(target: Annotated[str, TARGET_FIELD]) -> str:
    """Remove a widget from the editor canvas."""
    try:
        return json.dumps(_call("editor", "editor.delete", {"target": target}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_set_mode(
    editor_mode: Annotated[
        bool,
        Field(
            description=(
                "True: grid and selection chrome, frames capture clicks. False: "
                "frames become mouse-transparent and clicks reach the live widgets."
            )
        ),
    ],
) -> str:
    """Toggle the editor's edit/live mode.

    Worth knowing: in live mode the frames set WA_TransparentForMouseEvents, so a
    click aimed at a frame does nothing. ui_snapshot reports that flag.
    """
    try:
        return json.dumps(_call("editor", "editor.set_mode", {"editor_mode": editor_mode}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_save(path: Annotated[str, Field(description="Where to write the YAML.")]) -> str:
    """Save the editor canvas as a dashboard YAML config."""
    try:
        return json.dumps(_call("editor", "editor.save", {"path": path}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def editor_load(path: Annotated[str, Field(description="YAML config to open.")]) -> str:
    """Load a dashboard YAML config into the editor canvas."""
    try:
        return json.dumps(_call("editor", "editor.load", {"path": path}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


# --------------------------------------------------------------- widget config


@mcp.tool()
def widget_describe_config(
    app: Annotated[AppName | None, APP_FIELD] = None,
    type: Annotated[
        str | None,
        Field(
            default=None,
            description="Widget type name, e.g. 'mercedes_190e_speedometer'. Or pass target.",
        ),
    ] = None,
    target: Annotated[
        str | None, Field(default=None, description="A live widget to describe instead.")
    ] = None,
) -> str:
    """List a widget's settable fields: names, types, defaults, enum values, docs.

    Read this before widget_set_config rather than guessing field names -- an
    unknown field is rejected outright, and the descriptions come from what the
    widget's author wrote.
    """
    try:
        return json.dumps(
            _call(app, "widget.describe_config", {"type": type, "target": target}), indent=2
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def widget_get_config(
    target: Annotated[str, TARGET_FIELD],
    app: Annotated[AppName | None, APP_FIELD] = None,
) -> str:
    """Read a live widget's current configuration as JSON."""
    try:
        return json.dumps(_call(app, "widget.get_config", {"target": target}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def widget_set_config(
    target: Annotated[str, TARGET_FIELD],
    config: Annotated[
        dict[str, Any],
        Field(
            description=(
                "Only the fields to change. Everything you do not name keeps its "
                "current value. Nested structs take nested objects."
            )
        ),
    ],
    app: Annotated[AppName | None, APP_FIELD] = None,
    screenshot: Annotated[
        bool, Field(default=False, description="Also return a screenshot afterwards.")
    ] = False,
) -> list[Any]:
    """Change a live widget's configuration and rebuild it in place.

    The change is all-or-nothing: if any field is unknown or the wrong type, the
    call is rejected and nothing changes, with the problems listed. Geometry, id
    and position are preserved, so selectors and refs stay valid.
    """
    try:
        result = _call(app, "widget.set_config", {"target": target, "config": config})
    except (AgentError, LaunchError, OSError) as exc:
        return [_fail(exc)]

    parts: list[Any] = [json.dumps(result, indent=2)]
    if screenshot:
        try:
            shot = _call(app, "ui.screenshot", {"target": target})
            parts.extend(_screenshot_content(shot))
        except (AgentError, LaunchError, OSError) as exc:
            parts.append(_fail(exc))
    return parts


# ----------------------------------------------------------------------- zenoh


@mcp.tool()
def zenoh_list(
    app: Annotated[AppName | None, APP_FIELD] = None,
    key: Annotated[
        str, Field(default="**", description="Key expression filter, e.g. 'vehicle/**'.")
    ] = "**",
    window_ms: Annotated[
        int, Field(default=1000, description="How long to listen for traffic.")
    ] = 1000,
) -> str:
    """List the zenoh topics currently carrying traffic, with schema and rate.

    Topics are discovered by listening, because zenoh has no retained messages
    and no registry. An empty result means nothing published during the window,
    NOT that nothing exists -- a slow publisher looks the same as an absent one.
    Try a longer window_ms before concluding a topic is missing.
    """
    try:
        return json.dumps(
            _call(app, "zenoh.list", {"key": key, "window_ms": window_ms, "_timeout_ms": window_ms + 5000}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def zenoh_read(
    key: Annotated[str, Field(description="Exact zenoh key, e.g. 'vehicle/speed_mps'.")],
    app: Annotated[AppName | None, APP_FIELD] = None,
    timeout_ms: Annotated[
        int, Field(default=2000, description="How long to wait for a sample.")
    ] = 2000,
) -> str:
    """Read the next sample on a zenoh key, decoded to JSON.

    Works for every schema in the registry with no per-schema support, because
    the decode goes through Cap'n Proto's dynamic API using the schema name the
    publisher stamps on each sample.
    """
    try:
        return json.dumps(
            _call(app, "zenoh.read", {"key": key, "timeout_ms": timeout_ms, "_timeout_ms": timeout_ms + 5000}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def zenoh_publish(
    key: Annotated[str, Field(description="Zenoh key to publish on.")],
    schema: Annotated[
        str, Field(description="Schema name, e.g. 'VehicleSpeed'. See zenoh_describe_schema.")
    ],
    fields: Annotated[
        dict[str, Any],
        Field(description='Message fields, e.g. {"speedMps": 27.0, "timestamp": 0}.'),
    ],
    app: Annotated[AppName | None, APP_FIELD] = None,
) -> str:
    """Publish a message onto the vehicle bus. This is the fastest feedback loop.

    Set a value, screenshot the gauge that subscribes to it, and see whether the
    needle moved -- no test_data_publisher, no CAN bus, no car. Publishing stamps
    the same encoding a real publisher uses, so subscribers accept it and their
    schema checks stay quiet.

    Rejected all-or-nothing if any field is unknown or the wrong type, with the
    problems listed. Note it publishes to the whole bus, so anything else
    listening sees it too.
    """
    try:
        return json.dumps(
            _call(app, "zenoh.publish", {"key": key, "schema": schema, "fields": fields}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def zenoh_rate(
    key: Annotated[str, Field(description="Key expression to measure.")],
    app: Annotated[AppName | None, APP_FIELD] = None,
    window_ms: Annotated[int, Field(default=1000, description="Measurement window.")] = 1000,
) -> str:
    """Measure how fast a key is publishing. Useful for 'is this stream alive?'."""
    try:
        return json.dumps(
            _call(app, "zenoh.rate", {"key": key, "window_ms": window_ms, "_timeout_ms": window_ms + 5000}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def zenoh_describe_schema(
    app: Annotated[AppName | None, APP_FIELD] = None,
    schema: Annotated[
        str | None,
        Field(default=None, description="Schema name. Omit to list every known schema."),
    ] = None,
) -> str:
    """List the fields of a capnp schema, or every schema the build knows about.

    Read this before zenoh_publish rather than guessing field names.
    """
    try:
        return json.dumps(_call(app, "zenoh.describe_schema", {"schema": schema}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


# ----------------------------------------------------------------------- scope

# These are ergonomics, not capability. Every scope.* method is reachable
# through app_call from the moment the C++ side registers it; a typed wrapper
# buys a schema the model can see without asking, which is worth having for the
# verbs used constantly and not worth writing for the rest.

SCOPE_APP: AppName = "scope"


@mcp.tool()
def scope_panels() -> str:
    """List the scope's panels, what each plots, and the panel types available."""
    try:
        return json.dumps(_call(SCOPE_APP, "scope.panels"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_add_panel(
    type: Annotated[
        str, Field(default="time_series", description="Panel type; see scope_panels.")
    ] = "time_series",
    id: Annotated[
        str | None,
        Field(default=None, description="Stable id for the panel. Generated when omitted."),
    ] = None,
) -> str:
    """Add a panel to the scope window.

    New panels tab onto the last one rather than splitting the window, because a
    fifth panel in a four-way split is unreadable. Drag the tab to split.
    """
    params: dict[str, Any] = {"type": type}
    if id:
        params["id"] = id
    try:
        return json.dumps(_call(SCOPE_APP, "scope.add_panel", params), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_remove_panel(panel: Annotated[str, Field(description="Panel id.")]) -> str:
    """Remove a panel and release the signals it was subscribed to."""
    try:
        return json.dumps(_call(SCOPE_APP, "scope.remove_panel", {"panel": panel}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_browser() -> str:
    """List the topics and fields the scope can plot.

    Topics are listed from advertisements: every publisher declares a zenoh
    liveliness token when it starts, so a topic appears here whether or not it
    has ever published anything. There is nothing to rescan and no window to
    wait for -- an empty list means no publisher is running.

    A topic whose publisher has gone away stays listed and is marked
    unreachable, so a binding is never silently lost.
    """
    try:
        return json.dumps(_call(SCOPE_APP, "scope.browser"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_add_signal(
    panel: Annotated[str, Field(description="Panel id to add the signal to.")],
    zenoh_key: Annotated[str, Field(description="Topic, e.g. 'vehicle/engine/rpm'.")],
    field: Annotated[
        str | None,
        Field(default=None, description="Field name, e.g. 'rpm'. Resolved through the browser."),
    ] = None,
    schema: Annotated[
        str | None,
        Field(
            default=None,
            description=(
                "Schema name, e.g. 'EngineRpm'. Only needed when the browser has not seen "
                "the topic -- pass it to skip a scan."
            ),
        ),
    ] = None,
    type_category: Annotated[
        str | None,
        Field(default=None, description="capnp category ('float', 'uint', ...) when passing schema."),
    ] = None,
) -> str:
    """Plot a signal on a panel.

    The usual form names a topic and a field and lets the browser supply the
    rest, which needs a scan to have happened. Passing schema explicitly works
    before any scan.

    A panel refuses what it cannot show -- a time-series plot declines
    non-numeric fields and whole topics -- and the refusal names the candidate.
    """
    params: dict[str, Any] = {"panel": panel, "zenoh_key": zenoh_key}
    if field is not None:
        params["field"] = field
    if schema is not None:
        params["schema"] = schema
    if type_category is not None:
        params["type_category"] = type_category
    try:
        return json.dumps(_call(SCOPE_APP, "scope.add_signal", params), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_remove_signal(
    panel: Annotated[str, Field(description="Panel id.")],
    index: Annotated[int, Field(description="Zero-based index into the panel's traces.")],
) -> str:
    """Stop plotting one of a panel's signals."""
    try:
        return json.dumps(
            _call(SCOPE_APP, "scope.remove_signal", {"panel": panel, "index": index}), indent=2
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_browser_drag(
    panel: Annotated[str, Field(description="Panel id to drop onto.")],
    zenoh_key: Annotated[str, Field(description="Topic of the field to drag.")],
    field: Annotated[str, Field(description="Field name to drag.")],
) -> str:
    """Drag a signal from the browser onto a panel, exercising the drop path.

    Use scope_add_signal to just add a signal. Use this when the drag-and-drop
    behaviour itself is what you want to check: it drives the drop TARGET
    directly, because QDrag::exec() runs a nested loop over real platform events
    that synthesized ones cannot advance. Covers accept/reject and the drop
    handler; only the few lines inside exec() are left out.
    """
    try:
        return json.dumps(
            _call(
                SCOPE_APP,
                "scope.browser_drag",
                {"panel": panel, "zenoh_key": zenoh_key, "field": field},
            ),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_time_base(
    window_seconds: Annotated[
        float | None, Field(default=None, description="Seconds of history shown at once.")
    ] = None,
    mode: Annotated[
        str | None, Field(default=None, description="'live' or 'paused'.")
    ] = None,
    render_rate_hz: Annotated[
        int | None, Field(default=None, description="Redraw rate, 1-120.")
    ] = None,
    cursor: Annotated[
        float | None,
        Field(default=None, description="Shared cursor time. Pass with clear_cursor to unset."),
    ] = None,
    clear_cursor: Annotated[
        bool, Field(default=False, description="Remove the shared cursor.")
    ] = False,
) -> str:
    """Read or change the shared time base, and report what the source can do.

    Pausing freezes the view, not the data: buffers keep filling, so unpausing
    shows what arrived meanwhile rather than a gap. The cursor is shared across
    panels, so every panel reads out the same instant.

    Called with no arguments this just reports the current state.
    """
    params: dict[str, Any] = {}
    if window_seconds is not None:
        params["window_seconds"] = window_seconds
    if mode is not None:
        params["mode"] = mode
    if render_rate_hz is not None:
        params["render_rate_hz"] = render_rate_hz
    if clear_cursor:
        params["cursor"] = None
    elif cursor is not None:
        params["cursor"] = cursor
    try:
        return json.dumps(_call(SCOPE_APP, "scope.time_base", params), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_sample_stats(
    panel: Annotated[
        str | None, Field(default=None, description="Limit to one panel. All panels when omitted.")
    ] = None,
) -> str:
    """Report what each plotted signal has actually received.

    Counts, drops, the retained time span, and min/max/last. This is how you
    check a plot without looking at it: a screenshot shows a line, this says
    what the line is made of. `dropped` above zero means samples were discarded
    before reaching the plot, which makes the trace a lie about the data.
    """
    params: dict[str, Any] = {}
    if panel:
        params["panel"] = panel
    try:
        return json.dumps(_call(SCOPE_APP, "scope.sample_stats", params), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_panel_config(
    panel: Annotated[str, Field(description="Panel id.")],
    config: Annotated[
        dict[str, Any] | None,
        Field(
            default=None,
            description="Fields to change. Omit to read the current config instead.",
        ),
    ] = None,
    describe: Annotated[
        bool, Field(default=False, description="Return the config's field schema instead.")
    ] = False,
) -> str:
    """Read, describe or change a panel's configuration.

    Setting is partial but all-or-nothing: only the fields you name are touched,
    and an unknown field name is an error rather than a silent no-op. Changing
    the traces list rebinds the panel's signals.
    """
    try:
        if describe:
            return json.dumps(
                _call(SCOPE_APP, "scope.panel_describe_config", {"panel": panel}), indent=2
            )
        if config is None:
            return json.dumps(
                _call(SCOPE_APP, "scope.panel_get_config", {"panel": panel}), indent=2
            )
        return json.dumps(
            _call(SCOPE_APP, "scope.panel_set_config", {"panel": panel, "config": config}),
            indent=2,
        )
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def scope_workspace(
    path: Annotated[str, Field(description="Path to a workspace YAML file.")],
    save: Annotated[
        bool, Field(default=False, description="Save to the path instead of loading from it.")
    ] = False,
) -> str:
    """Load or save a scope workspace.

    A workspace is the panels, what each plots, the time base and the dock
    arrangement. Loading replaces everything currently open.
    """
    try:
        method = "scope.save" if save else "scope.load"
        return json.dumps(_call(SCOPE_APP, method, {"path": path}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


# ------------------------------------------------------------------ escape hatch


@mcp.tool()
def app_call(
    method: Annotated[
        str, Field(description="RPC method name, e.g. 'ui.snapshot'. See app_methods.")
    ],
    app: Annotated[AppName | None, APP_FIELD] = None,
    params: Annotated[
        dict[str, Any] | None, Field(default=None, description="Method parameters.")
    ] = None,
) -> str:
    """Call any control-interface method directly.

    An escape hatch for methods this server has no typed wrapper for yet --
    including any the application registered for itself. Use app_methods to see
    what the running build supports.
    """
    try:
        return json.dumps(_call(app, method, params or {}), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)


@mcp.tool()
def app_methods(app: Annotated[AppName | None, APP_FIELD] = None) -> str:
    """List the RPC methods the running application supports."""
    try:
        return json.dumps(_call(app, "rpc.methods"), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _fail(exc)
