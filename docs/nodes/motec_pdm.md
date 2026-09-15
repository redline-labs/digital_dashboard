---
title: motec_pdm
parent: Nodes
---

# motec_pdm

## Overview

`motec_pdm` decodes a MoTeC PDM's Generic Output CAN frames into seven typed
telemetry topics: input states, output currents, loads, voltages and statuses,
input voltages, and the unit's serial and firmware. It reads the raw frames
[can_bridge](can_bridge.html) publishes and decodes them with the generated
parser for `dbcs/motec/PDM_Generic_Output.dbc`. It needs can_bridge running
with the PDM on the channel it subscribes to. It does not configure the PDM
and does not drive its outputs.

## Running it

The node takes command-line options only; there is no configuration file.

```bash
./build/nodes/motec_pdm/motec_pdm
./build/nodes/motec_pdm/motec_pdm --source vehicle/can1/rx --prefix nodes/pdm_rear
```

| Option | Default | Meaning |
|---|---|---|
| `-s`, `--source <key>` | `vehicle/can0/rx` | The `CanFrame` topic to read |
| `-p`, `--prefix <key>` | `nodes/motec_pdm` | Where the seven topics live; give a second PDM its own |
| `-h`, `--help` | | Print usage and exit |

### Running without hardware

There is no PDM simulator in the tree. Feed the node from a recorded trace
through can_bridge's `trc:` channel (see `configs/can_bridge/replay.yaml`), a
[bag](bag.html) recording, or `inspect publish` on the source key with schema
`CanFrame`. Every message here is multiplexed into pages, and a topic publishes
only once every page of its message has been seen, so a single hand-made frame
produces nothing; a full set of pages for one id does.

## Topics

With the default prefix:

| Topic | Schema | From CAN id | Contents |
|---|---|---|---|
| `nodes/motec_pdm/input_state` | `MotecPdmInputState` | `1280` (`0x500`) | Reset source, 9.5 V rail, total current in A, global error flag, battery volts, internal temperature in C, and 23 input flags |
| `nodes/motec_pdm/output_current` | `MotecPdmOutputCurrent` | `1281` (`0x501`) | 32 output currents in A |
| `nodes/motec_pdm/output_load` | `MotecPdmOutputLoad` | `1282` (`0x502`) | 32 output loads in % |
| `nodes/motec_pdm/output_voltage` | `MotecPdmOutputVoltage` | `1283` (`0x503`) | 32 output voltages in V |
| `nodes/motec_pdm/output_status` | `MotecPdmOutputStatus` | `1284` (`0x504`) | 32 output statuses as `PdmOutputStatusEnum` |
| `nodes/motec_pdm/input_voltage` | `MotecPdmInputVoltage` | `1285` (`0x505`) | 23 input voltages in V |
| `nodes/motec_pdm/info` | `MotecPdmInfo` | `1285` (`0x505`) | Serial number low and high bytes, firmware major, minor and letter |

`info` and `input_voltage` come from the same message, `0x505`, whose last
page carries the serial and firmware; both publish together. An output status
is one of `off`, `on`, `faultError`, `overCurrentError` or `retriesReached`;
any other raw value is published as `off`.

## Services

None.

## Troubleshooting

**Every topic is advertised and none of them publishes.** Advertisement
happens at startup, so `inspect list` only says the node is up. `inspect hz
<source>` says whether frames arrive; none means can_bridge is not running or
the PDM is on a channel whose topic is not the one `--source` names. Frames
arriving but none in `1280` to `1285` means the PDM's Generic Output is
configured at a different base, or not enabled, in PDM Manager; only base
`0x500` is decoded.

**Some ids arrive and a topic still stays silent.** Each message publishes
only when all its multiplex pages have been seen since the last publish.
`inspect echo <source> -n 50` and check the first data byte of that id cycles
through its page numbers; a message stuck on one page never completes.

**The bus is empty and `vehicle/can/status` shows error counters climbing or
the channel bus-off.** That is a bit-rate mismatch presenting as silence.
can_bridge's shipped config runs `can0` at 500 kbit/s; MoTeC devices are often
at 1 Mbit/s, and the PDM's rate is set in PDM Manager.

**Two PDMs, one set of topics.** Both are publishing on the same prefix and
their samples interleave. Run one node per PDM with distinct `--prefix` values,
and distinct `--source` keys if they are on different channels; if they share a
channel and a base id, nothing downstream can tell them apart.
