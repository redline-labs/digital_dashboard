---
title: racegrade_tc8
parent: Nodes
---

# racegrade_tc8

## Overview

`racegrade_tc8` decodes a RaceGrade TC8 thermocouple amplifier's CAN frames
into two typed telemetry topics: its inputs (eight voltages, eight
thermocouple temperatures, four frequencies) and its diagnostics. The TC8
transmits in the MoTeC E888 expander format, so the node uses the generated
parser for `dbcs/motec/Motec_E888_Rev1.dbc`. It reads the raw frames
[can_bridge](can_bridge.html) publishes and needs can_bridge running with the
TC8 on its `can0` channel. It does not configure the device: the one service
it offers is a placeholder that acknowledges and sends nothing.

## Running it

The node takes command-line options only; there is no configuration file. The
frame topic and the output topics are fixed in the source.

```bash
./build/nodes/racegrade_tc8/racegrade_tc8
```

| Option | Meaning |
|---|---|
| `--debug` | Debug logging |
| `-h`, `--help` | Print usage and exit |

### Running without hardware

`mock_racegrade_tc8_frames` under `mock_data/` publishes a full cycle of the
five `Inputs` pages every 75 ms or so, with every channel tracing a sine wave,
on the topic the node reads.

```bash
./build/mock_data/mock_racegrade_tc8_frames                 # -k to use another key
./build/nodes/racegrade_tc8/racegrade_tc8 &
./build/nodes/inspect/inspect echo nodes/racegrade_tc8/inputs -n 3
```

The mock sends no `Diagnostics` frames, so `diagnostics` stays silent under
it. `test_racegrade_tc8_query`, built next to the node, calls the placeholder
service once and prints the reply.

## Topics

| Topic | Schema | Contents |
|---|---|---|
| `nodes/racegrade_tc8/inputs` | `RaceGradeTc8Inputs` | `voltage1..8` in V, `temperature1..8` in C, `frequency1..4` in Hz |
| `nodes/racegrade_tc8/diagnostics` | `RaceGradeTc8Diagnostics` | Two cold-junction compensation temperatures, internal temperature, six digital input states, battery volts, status flags, firmware version |

The node subscribes to `vehicle/can0/rx` with schema `CanFrame` and decodes
two ids:

| CAN id | Message | Pages |
|---|---|---|
| `240` (`0x0F0`) | `Inputs` | Five, selected by the top three bits of byte 0: two voltages and two temperatures per page, with the frequencies on pages 3 and 4 |
| `241` (`0x0F1`) | `Diagnostics` | Three, selected by the top four bits of byte 0 |

A topic publishes once every page of its message has been seen since the last
publish, so each sample holds one complete cycle. Temperatures are in quarter
degrees on the wire and arrive here as C.

### Health

The node publishes `nodes/racegrade_tc8/health` ([NodeHealth](../libs/node_health.html))
once a second, and at once whenever a check changes:

| Check | Degraded when |
|---|---|
| `can_rx` | no CAN frame of any kind has arrived for a second: the bus is quiet, or the node is subscribed to the wrong key |
| `decoded` | frames are arriving but none of them has been one of this node's messages for two seconds |

`inspect health` prints them. The node exits cleanly on SIGINT or SIGTERM, so a
monitor reads a deliberate stop as `exited` rather than as a crash.

## Services

| Key | Request | Response |
|---|---|---|
| `nodes/racegrade_tc8/hello` | `RaceGradeTc8ConfigureRequest` | `RaceGradeTc8ConfigureResponse` |

The request names a message format (`e888Id0x0F0` to `e888Id0x0FC`, or a
user-selectable output in temperature or millivolts), a transmit rate from
50 Hz down to once a minute, and a CAN id. As of 2026-09-14 the handler logs
those three fields and replies `response: true` without transmitting anything
to the TC8, so calling it changes nothing on the device.

## Troubleshooting

**Both topics are advertised and neither publishes.** `inspect hz
vehicle/can0/rx` first: no frames means can_bridge is not running, or the TC8
is on a channel whose topic is not `vehicle/can0/rx`, and this node cannot be
redirected. Frames arriving but none on `240` or `241` means the TC8 is set to
another E888 base. The format enum in the service request lists `0x0F4`,
`0x0F8` and `0x0FC` as alternatives the device supports; only `0x0F0` and
`0x0F1` are decoded here.

**`inputs` publishes and `diagnostics` never does.** Under the mock that is
expected. On hardware it means no `0x0F1` frames, or not all three of its
pages: `inspect echo vehicle/can0/rx -n 50` and look for id `241` with byte 0
cycling through `0x00`, `0x10`, `0x20`.

**The bus is empty and `vehicle/can/status` shows error counters climbing.**
A bit-rate mismatch presents as silence, not as bad values. can_bridge's
shipped config runs `can0` at 500 kbit/s; MoTeC-format devices commonly run
at 1 Mbit/s.

**A temperature channel reads a large constant.** An open thermocouple is
reported by the device, not by this node; compare the cold-junction values in
`diagnostics`, which should be near ambient, to tell a device fault from a
wiring one.
