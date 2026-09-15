---
title: Apps
nav_order: 2
---

# Apps

Four GUI programs, all under `apps/` in the tree. They share the bus, the
reflected configuration machinery and the agent control interface, and nothing
else: each is its own executable with its own page.

| App | What it is for | Page |
|---|---|---|
| `dashboard` | Renders a layout: the instrument cluster itself, on one or more displays. | [dashboard](dashboard/) |
| `editor` | Edits a layout by dragging widgets onto a canvas and setting their properties. | not written yet |
| `scope` | Watches any signal on the bus, live or from a recording, as plots, tables, video and a map. | [scope](scope.html) |
| `switchboard` | Lists every service advertised on the bus and calls one from a form built from its request schema. | [switchboard](switchboard.html) |

All four take `--mcp` to expose a control socket so they can be run headless and
driven by an agent; see [Agent control](../developing/agent-control.html).
