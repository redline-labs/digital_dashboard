---
title: Design notes
nav_order: 7
---

# Design notes

## Overview

Working notes on why things are shaped the way they are: what was tried, what
was measured, and what the hardware turned out to do. They keep their numbers
and dates, and they are not maintained as manuals. When a page in Apps, Nodes,
Tools or Libraries needs the history behind a decision, it links here.

| Note | What it covers | Reader-facing pages |
|---|---|---|
| [CarPlay port](carplay-port.html) | The port from LIVI, the USB transport, usbmux and lockdown, iAP2 and the MFi coprocessor, video, touch, audio, hardware sessions, and what is still unverified. | [carplay](../nodes/carplay.html) |
| [Map renderer](map-renderer.html) | Why the renderer is ours, how it draws, the pitched camera, labels, the tile cache, and what a frame costs. | [map_server](../nodes/map_server.html), [map_render](../libs/map_render.html) |
| [Map build](map-build.html) | Why it replaced tilemaker, the one classification table, memory, the tiler's geometry traps, identity, and how the routing hierarchy is trusted. | [map_build](../tools/map_build.html) |
| [Race tracks](tracks.html) | What the source data is, deriving centrelines, the QA gate, and known gaps. | [tracks](../tools/tracks.html) |
| [scope internals](scope-internals.html) | The data source seam, panel registration, buffering and seeking, capture, the recorded path, and the per-panel decisions. | [scope](../apps/scope.html) |
| [bag format](bag-format.html) | The MCAP layout, the two timestamps, drops, and reading a damaged recording. | [bag](../nodes/bag.html) |
| [MoTeC UTC protocol](motec-utc-protocol.html) | The reverse-engineered protocol, the `0x21` latch, and what has been verified. | [can_motec](../libs/can_motec.html), [can_bridge](../nodes/can_bridge.html) |
| [Trimble BD992 notes](bd992.html) | ICD gaps, why the parsers are `constexpr`, and live-hardware findings. | [bd992_bridge](../nodes/bd992_bridge.html), [gsof](../libs/gsof.html) |
| [Xsens MTi-610 notes](mti610.html) | The protocol's traps, documentation gaps, and what waits on hardware. | [mti610_bridge](../nodes/mti610_bridge.html), [xbus](../libs/xbus.html) |
| [MOTOTRBO notes](mototrbo.html) | The handshake defects and where the protocol knowledge came from. | [xpr_bridge](../nodes/xpr_bridge.html), [mototrbo](../libs/mototrbo.html) |
