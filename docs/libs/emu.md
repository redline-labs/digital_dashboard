---
title: emu
parent: Libraries
---

# emu

## Overview

What every emulator under [sims](../sims/index.html) is wired from: simulated
time in ticks, the electrical net between parts, interrupt lines, a CAN bus
between simulated controllers, snapshot I/O, and the record of where a model is
known to be incomplete. It has no CPU and no device in particular;
[hc08](hc08.html) and [gz60](gz60.html) are built on it, and a simulator
assembles a product from those.

Header-only, CMake target `emu`.

## Public headers

| Header | |
| --- | --- |
| `emu/common.h` | The integer and tick types everything shares. |
| `emu/clock.h`, `emu/net.h`, `emu/part.h` | Time, the voltage on a net, and a part that drives or reads one. |
| `emu/irq_line.h` | An interrupt line a device raises and a controller sinks. |
| `emu/can_bus.h` | `CanBus`, `CanNode`, the bus `Tap` and `BusFault`. |
| `emu/snapshot_io.h` | `put_value`/`get_value` for trivially copyable state. |
| `emu/model_gap.h` | `model_gap::warn`: logged once per site, the first time a run touches something the model does not implement. |

## Behaviour worth knowing

The frame on a simulated bus is the tree's own `helpers::CanFrame`, so what a
simulated controller transmits goes onto a real bus or onto zenoh as it is. A
classic controller model carries `len` 0 to 8; a DLC of 9 to 15, which still
means eight bytes on classic CAN, is not distinguished from 8.

The bus is instantaneous: it hands a frame to every other node in the order they
were attached. A controller that models bus time (arbitration, frame length,
errors) does so on its own side.

## Tests

Exercised through [hc08](hc08.html#tests) and [gz60](gz60.html#tests), which use
every header here.
