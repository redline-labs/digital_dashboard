---
title: mpc55xx
parent: Libraries
---

# mpc55xx

## Overview

The MPC5554 around an [e200](e200.html) machine: internal flash and SRAM, the
peripheral bridges, and the modules the M150 simulator needs, with storage stubs
for the rest. Which MPC55xx the M150 carries is not confirmed; the MPC5554's map
is the default.

The part's boot ROM (BAM) is not in the tree and does not run. `bam_boot()` does
its work for a boot from internal flash: the five TLB entries it leaves, the
reset configuration half word checked for its boot identifier, and a jump to the
reset vector after it.

## Public headers

| Header | |
| --- | --- |
| `mpc55xx/soc.h` | `Mpc5554`: the memory map, the modules, `load_flash`, `reset`/`bam_boot`; the INTC source numbers of the FlexCAN buffers. |
| `mpc55xx/intc.h` | `Intc`: software vector mode, priorities, the acknowledge and end-of-interrupt LIFO, software interrupts. |
| `mpc55xx/flexcan.h` | `FlexCan`: freeze and disable, bit timing, 64 buffers, transmit by ID or buffer, receive through the masks, flags and interrupts. |
| `mpc55xx/esci.h` | `Esci`: a console; what is written to the data register goes to `on_byte`. |
| `mpc55xx/registers.h` | `RegisterFile`, and `StubModule` (storage, first write logged as a model gap). |

## Behaviour worth knowing

A FlexCAN transmission takes its frame's time on the wire (without stuff bits) at
the programmed bit rate; a received frame lands in the first empty matching
buffer, else overruns the last full matching one. Error counters and states are
not modelled.

The INTC runs in software vector mode only; setting hardware vector mode is
logged as a model gap. The FlexCAN interrupt source numbers are from the
MPC5554 reference manual's table and have not been checked against hardware.

eSCI is a console only: no reception, no baud rate.

## Tests

| Target | Proves |
| --- | --- |
| `mpc55xx_test_soc` | A bare image assembled in the test: boot from the reset configuration (and none without one), the console, a FlexCAN transmit at its bit rate, and receive through the mask with an interrupt-driven echo through the INTC. |
