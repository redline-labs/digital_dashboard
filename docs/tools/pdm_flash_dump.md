---
title: pdm_flash_dump
parent: Tools
---

# pdm_flash_dump

## Overview

Reads a MoTeC PDM32's flash and RAM over CAN, read-only, through the same
comms protocol PDM Manager uses. It opens any channel the CAN backends offer, so
the same binary reads a unit on an adapter here, a unit on a vehicle's bus
through [can_bridge](../nodes/can_bridge.html) and `zenoh:`, or
[pdm32_sim](../sims/pdm32.html). It also turns a dumped current calibration into
the header the simulator compiles in.

{: .important }
What it reads off a unit is MoTeC's and the unit owner's. It goes in `dumps/`,
which is ignored, and stays out of the tree.

## Running it

```bash
build/tools/pdm_flash_dump/pdm_flash_dump dump --channel pcan:0 --out dumps/unit/
build/tools/pdm_flash_dump/pdm_flash_dump dump --channel zenoh:vehicle/can0 --out dumps/sim/
```

| Option | Meaning |
|---|---|
| `--channel` | Any channel id: `pcan:0`, `socketcan:can0`, `motec:...`, `zenoh:<prefix>`. |
| `--out` | The output directory. |
| `--serial` | The unit's serial. Without it the tool waits for the unit's discovery beacon. |
| `--bitrate` | Set the adapter to this rate first. The PDM32 normally runs at 1 Mbit/s. |
| `--timeout-ms` | How long to wait for a reply. Default 1000. |
| `--with-rom` | Also read the monitor ROM. |
| `--range FROM-TO` | Only these ranges, hex and inclusive; repeatable. Each must lie in RAM, flash or ROM. |

## Output

| File | What it is |
|---|---|
| `image.s19` | Every byte read, as S-records. Gaps not read are absent. |
| `manifest.json` | The serial, the ranges read, the request types sent, the reads retried, and each block's check. |
| `config.bin` | The configuration block, when its header is good. |
| `current_cal.bin` | The current calibration, when its header is good. |
| `factory.bin` | The factory block. |
| `bootloader.bin` | The bootloader, which is not in PDM Manager's image. |

## Read-only

The tool can make three kinds of request: open a comms session, read memory,
and the flow control a read needs. Those are the only values of the type its
frames are built from, so a request that writes or erases cannot be expressed,
not merely refused. Opening a session changes only the unit's volatile comms
state, and the tool hands it back at the end; the unit also returns to its
defaults on a power cycle. Reads are confined to RAM, flash and the monitor ROM,
because reading some peripheral registers clears status flags.

A read that stalls (the unit drops a read whose flow control arrives late) is
asked again, which a read allows because it has no side effects. A reply out of
sequence, a refused read, or more data than was asked for stops the dump.

## The calibration header

```bash
build/tools/pdm_flash_dump/pdm_flash_dump cal-header dumps/unit/current_cal.bin \
    > sims/pdm32/config/current_cal_dumped.h
```

It refuses a block whose header, length or CRC is wrong.
`pdm_flash_dump_test_cal_header` holds the generator, the simulator's encoder and
the committed header to each other byte for byte.

## Tests

| Target | Proves |
| --- | --- |
| `pdm_flash_dump_test_session` | The session against a fake unit: discovery, connect, reads of every length, only read-only requests on the wire, refused ranges sent nothing, retries after a stall, and a unit that answers out of order, refuses, or sends too much. |
| `pdm_flash_dump_test_cal_header` | The round trip above, and bad blocks refused. |
| `pdm_flash_dump_test_sim` | A dump of `pdm32_sim` over `zenoh:` returns the images the simulator loaded, and the simulated unit's flash is untouched (slow). |

{: .note }
As of 2026-11-08 this C++ tool has been run against the simulator only. The
Python tool it replaces read a real unit.
