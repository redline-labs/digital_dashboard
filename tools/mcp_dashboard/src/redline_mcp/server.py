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
# two-valued enum, which makes a second instance unrepresentable in the schema;
# this says why, so the constraint reads as intentional rather than as a gap.
APP_FIELD = Field(
    default=None,
    description=(
        "Which app to target: 'dashboard' or 'editor'. Optional when only one is "
        "running. There is at most one instance of each type."
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
Drives the Redline dashboard and editor headlessly (Qt 'offscreen' platform) so
you can see and interact with them without a human at the screen.

AT MOST ONE `dashboard` AND ONE `editor` RUN AT A TIME. There is no support for
running two dashboards: instances share a zenoh bus, so a second one would
observe samples injected at the first. `app_launch` on a type that is already
running REPLACES that instance (any unsaved editor state is lost). To run a
dashboard and an editor together, call `app_launch` once per type.

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
        parts.append(Image(data=base64.b64decode(encoded), format="png"))
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
