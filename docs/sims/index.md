---
title: Simulators
nav_order: 3.5
---

# Simulators

A simulator under `sims/` stands in for a piece of hardware: the device's own
firmware on an emulated microcontroller, its board, and what is wired to its
connector. It puts the device on the same topics, in the same schema, as the
node that would bridge the real one, so the nodes, apps and tools downstream run
unchanged against it. That makes it the place to develop a configuration, a
decoder or a dashboard page before the hardware is on the bench.

Simulators run on a workstation and never ship, like `tools/`. The firmware they
run belongs to the device's maker and is never in the tree: each simulator's page
says where it looks for it.

| Simulator | Stands in for | Needs | Page |
|---|---|---|---|
| `pdm32_sim` | A MoTeC PDM32 power distribution module on a CAN bus, as `can_bridge` would put a real one on `vehicle/can0`. | the PDM32 application image from PDM Manager | [pdm32_sim](pdm32.html) |
| `m150_sim` | A MoTeC M150 ECU running an M1 application, its three CAN buses on zenoh as `can_bridge` would put a real unit's. | a flash image built outside the tree (an MVP kernel and an M1 application) | [m150_sim](m150.html) |

Every simulator's `serve` is the same host (`sims/host`): its config binds each
of the device's CAN ports to a bus, as `can_bridge` binds an adapter's
channels. A device with one port takes a mapping (`can: {name: can0}`); one
with several names them (`can: {CAN1: {name: can0}, CAN2: off}`).

Every simulator also has the same debugger (`sims/host/debug`), reached through
its agent methods and the MCP `sim_*` tools:
- running and stepping: `run`, `step` (and over a call), `finish`;
- stopping: breakpoints by function or address, watches on memory (stopping or recording), with conditions;
- reading: values by name (registers, symbols at 8, 16 or 32 bits or as a float, addresses, and the device's own names), registers, disassembly from the same tables the CPU executes, a backtrace, memory;
- snapshots of the whole device.

A simulator provides a `DebugTarget` (its CPU's registers, memory, decoder,
stack and snapshots); the debugger, the session and the methods are written
once over it.
