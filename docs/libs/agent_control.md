---
title: agent_control
parent: Libraries
---

# agent_control

## Overview

The agent-facing control endpoint embedded in the dashboard, editor, scope and
switchboard. `AgentServer` speaks JSON-RPC 2.0 over newline-delimited frames on
a unix-domain socket and runs every registered handler on the GUI thread, so a
handler may touch widgets directly. Around it sit the pieces a handler needs:
a selector resolver, a widget snapshot, screenshot capture, synthesized input,
a queryable log ring, and the `zenoh.*` methods.

It deliberately does not speak MCP. Tool names, descriptions and argument
schemas live in the companion Python server under `tools/mcp_dashboard`, where
they change without a rebuild; what lives here is only the set of things the
app can do. It also knows nothing about dashboard widgets: the `widget.*`
methods sweep the widget table and so live in
[dashboard_widgets](dashboard_widgets.html). The library is not named `mcp_*`
because `libs/mcp2221a` is a USB-to-I2C bridge driver and the collision would
be confusing; the user-facing flag is still `--mcp`.

[Agent control](../developing/agent-control.html) is the page for using the
interface: the method list, the selector grammar, the coordinate contract and
the debugging loop. This page is about linking and extending the library.

## Public headers

| Header | |
| --- | --- |
| `agent_control/server.h` | `AgentServer`: `registerMethod`, `start`, `stop`, `handleLine`, the shared `WidgetLocator`, `MethodKind`. |
| `agent_control/methods.h` | `registerCoreMethods()`: `ui.*`, `input.*`, `app.info`, `app.quit`, and `AppInfo`. |
| `agent_control/zenoh_methods.h` | `registerZenohMethods()`: `zenoh.list`, `read`, `publish`, `rate`, `describe_schema`. |
| `agent_control/error.h` | `ErrorCode`, `AgentError`, `Result<T>`, `MethodResult`, and the `badParams`-style builders. |
| `agent_control/locator.h` | `WidgetLocator`: selectors to live widgets, stable refs, the tree revision. |
| `agent_control/inspector.h` | `buildSnapshot()` and `describeWidget()`: the flat widget list. |
| `agent_control/capture.h` | `captureWidget()` and `CaptureOptions`: the PNG plus the metadata that fixes its coordinates. |
| `agent_control/input.h` | `sendClick`, `sendDrag`, `sendDrop`, `sendKeySequence`, `sendText`, all widget-local. |
| `agent_control/gui_thread.h` | `callOnGuiThread()` with a timeout, and `settleEventLoop()`. |
| `agent_control/log_sink.h` | `RingSink`, `installLogCapture()`, `logRing()`. |
| `agent_control/control_socket.h` | `ControlSocket`: the accept loop and per-client threads. Never touches Qt. |

## Using it

Link the CMake target `agent_control`. The dashboard's `main()` builds the
server, registers the shared methods, adds its own, and starts it:

```cpp
auto agent = std::make_unique<agent_control::AgentServer>("dashboard");

agent_control::AppInfo app_info;
app_info.app = "dashboard";
app_info.config_path = selection->path;
agent_control::registerCoreMethods(*agent, app_info);
dashboard::agent::registerWidgetMethods(*agent, applier);
agent_control::registerZenohMethods(*agent);

agent->registerMethod("dashboard.reload",
    [&](const json& params) -> agent_control::MethodResult { /* on the GUI thread */ },
    agent_control::AgentServer::MethodKind::kMutating);

agent->start(*args->mcp_socket_path);
```

Mark a handler `kMutating` when it changes state; the dispatcher then drains
the event loop before answering, so a screenshot taken straight after observes
the effect rather than the previous frame.

## Behaviour worth knowing

Handlers are posted to the GUI thread with a timeout, never with
`Qt::BlockingQueuedConnection`, which waits forever. A wedged GUI thread
therefore comes back as `GUI_THREAD_BUSY`, an answer about the app's health,
instead of hanging the caller at the moment it was built to diagnose. The
default budget is 5000 ms and `params["_timeout_ms"]` overrides it per call.
The accept loop runs on its own threads and never touches Qt for the same
reason. A line longer than 8 MB is refused rather than buffered.

Every `ErrorCode` is something a caller can act on differently: an ambiguous
selector means "say which", a stale ref means "re-snapshot". A selector that
matches nothing or more than one widget is always an error carrying the
candidates; a silent first match is how an agent drives the wrong widget and
reports a confident wrong conclusion.

{: .note }
`include/agent_control/server.h` is listed as a source in the CMakeLists so
that AUTOMOC sees its `Q_OBJECT`. Headers under `include/` are otherwise
skipped by the moc scan, and the symptom is a link error on
`staticMetaObject` that points nowhere near the cause. `find_package(Qt6)` is
repeated in this directory because imported targets are directory-scoped.

`RingSink` is not spdlog's `ringbuffer_sink`: that stores formatted strings
with no cursor. The monotonic `seq` is the point, so `since_seq` returns only
what is new and eviction is reported as `dropped`. `installLogCapture()` also
routes Qt's own diagnostics into spdlog under the logger `qt`; a screenshot
that comes back black often has a QPA line behind it. spdlog is a PUBLIC link
dependency because `log_sink.h` exposes a sink type.

The `zenoh.*` methods go through Cap'n Proto's dynamic API and the generated
schema registry, so a schema added to `schemas/` works with no change here.
They share `capnp_json.h` and `topic_discovery.h` with `nodes/inspect`, so the
two cannot disagree about what is on the bus.

## Tests

| Target | Labels | Proves |
| --- | --- | --- |
| `agent_control_test_framing` | `agent_control unit` | The JSON-RPC envelope through the real `handleLine`, weighted towards truncated lines, wrong-typed fields, unknown parameters and throwing handlers. |
| `agent_control_test_log_ring` | `agent_control unit` | `seq` is monotonic and never reused, `since_seq` returns only what is new, eviction is reported, and Qt messages arrive. |
| `agent_control_test_selector` | `agent_control gui` | Selector resolution against a real widget tree fails loudly on ambiguity, staleness and out-of-range indices. |
