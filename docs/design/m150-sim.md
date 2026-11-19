---
title: M150 simulator
parent: Design notes
---

# M150 simulator

## Overview

`m150_sim` will stand in for a MoTeC M150 (an M1 "ecu120"): an MPC55xx on an e200 core, the board around it, and its
three CAN buses on zenoh as `can_bridge` would put a real unit's. It is the second simulator after
[pdm32_sim](../sims/pdm32.html), and the first on a different CPU, so it is also what shapes the simulators to take
more CPUs and board topologies.

The simulator is the hardware only. What runs on it -- a kernel and an M1 application -- is an image built and kept
outside the tree, as the PDM's firmware is; nothing built for the target is in the repository, and neither is a
toolchain for it.

## Decisions

| Question | Decision |
|---|---|
| Scope | One M150 by itself. Several devices on one bus come later and are assumed to run in real time, each its own process on its own topics. |
| CPU | Our own e200 core in `libs/e200`, built like `libs/hc08`: interpreter first, block engine and JIT later. |
| Kernel | Not in the simulator and not in the tree: the image brings its own. |
| Vendor material | None in the tree. The board's topology is ours: `board/topology/m150.cpp`, first derived from the M1 system's hardware description, now the source of truth and maintained by hand. |
| Part | The MPC5554 memory map by default, as a variant: which MPC55xx the M150 carries is not confirmed. |
| Time slice | 100 µs. |

## Layout

| Where | What |
|---|---|
| `libs/emu` | CPU-neutral: `Timed` devices and the `Schedule` that syncs them (Input first, repeated passes), clocks, nets, IRQ lines, the CAN bus. |
| `libs/hc08`, `libs/gz60` | The PDM's CPU08 and its MCU. `hc08::Periph` is a `Timed` with a register window. |
| `libs/e200` (to come) | The e200: user and supervisor ISA, the SPE's scalar single precision, exceptions (IVPR/IVORn), timebase and decrementer, the MMU (TLB1), a 32-bit physical bus. |
| `libs/mpc55xx` (to come) | The SoC: memory map, INTC, FlexCAN A-C on `emu::CanBus`, SIU, and register maps that log model gaps for the rest (eQADC, eMIOS, eTPU, DSPI, EBI, FMPLL). |
| `sims/host` | Every `serve`: the loop, CAN ports bridged onto zenoh, the serve config, agent-method plumbing. A device plugs in as a `ServedDevice`. |
| `sims/m150` | `board/` (the board, `board/topology/`), `plant/`, `top/` (instance, session, serve, methods), layered as `sims/pdm32` is. |

## The board's topology

`board/topology/` holds the M150's hardware description as constant tables (`topology.h` is the schema):

- the family's io features (din, dout, ain, cfg, core) with their methods, properties, enums and the 76 functions --
  72 numbered, the low byte of the hardware-function command the firmware passes;
- 35 io types and 5 driver devices (eqadc, ad7490, emios, tpu, comms);
- the M150's 4 connectors, 287 I/O with their pins, the devices' channel maps (a channel serves an I/O or a whole
  group: `INJ_PH_GLOBAL`, `CAN1`), the 10 task events (1 s .. 1 ms, the trigger bits), 4 init/exit hooks, and its
  characteristics (125 MHz system clock, tick shift 8, 10.416 MHz timer clock).

The tables are maintained by hand; `tests/test_topology.cpp` checks that every name one table uses resolves in
another, so an edit that leaves a dangling reference fails the build's tests.

## Phases

| Phase | Done when |
|---|---|
| 1. Shared infrastructure, the PDM moved onto it | Done: `libs/emu` `Timed`/`Schedule`, `sims/host`, the PDM on them with its tests, serve and the MCP tools unchanged. |
| 2. `libs/e200` interpreter and JIT | Done: the core, MMU, SPE and machine with interpreter, block and AArch64 JIT engines; tests from programs assembled in the tests, and the engines held to the interpreter on random programs. |
| 3. `libs/mpc55xx` and the M150 board | A bare image echoes CAN frames through FlexCAN and the INTC onto zenoh. |
| 4. `m150_sim serve` | An outside image (kernel and application) boots and puts its traffic on `vehicle/can0`; tests skip without it. |
| 5. Engine speed, I/O | Block chaining and an x86-64 JIT backend; the I/O resources from the topology driven by a plant. |
