---
title: Tools
nav_order: 4
---

# Tools

Programs under `tools/` run on a workstation and never ship to the vehicle.
That boundary is what keeps a tool that holds tens of gigabytes of scratch state
out of a deployment manifest.

| Tool | What it does | Page |
|---|---|---|
| `map_build` | OpenStreetMap PBF in; vector tiles, a routable road graph and a routing overlay out. Also builds the race-track tileset. | [map_build](map_build.html), [tracks](tracks.html) |
| `estimator_offline`, `estimator_sim` | Record a simulated drive with its truth, and run the state estimator over any bag both as the node would (fixed-lag) and as a whole-drive solve, for scope to compare. | [estimator_offline](estimator_offline.html) |
| `mcp_dashboard` | The MCP server that lets an agent launch and drive the GUI apps headless. Python, run with `uv`. | [Agent control](../developing/agent-control.html) |
