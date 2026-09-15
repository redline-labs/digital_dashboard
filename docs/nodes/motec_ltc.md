---
title: motec_ltc
parent: Nodes
---

# motec_ltc

## Overview

`motec_ltc` decodes a MoTeC LTC lambda-to-CAN module's frames into one typed
telemetry message per complete transmission. It reads the raw frames
[can_bridge](can_bridge.html) publishes, decodes them with the generated parser
for `dbcs/motec/Motec_LTC_Rev1.dbc`, and publishes `MotecLtcTelemetry`. It
needs can_bridge running with the LTC on its `can0` channel. It does not
configure the module and decodes exactly one module, the one the DBC calls
`LTC_1`.

## Running it

The node takes only command-line options. The frame topic it reads and the
prefix it publishes under are both configurable; the defaults are what a
standard bring-up uses.

```bash
./build/nodes/motec_ltc/motec_ltc
```

| Option | Meaning |
|---|---|
| `-s`, `--source` | Zenoh key carrying CAN frames (default `vehicle/can0/rx`) |
| `-p`, `--prefix` | Key prefix for this node's topics (default `nodes/motec_ltc`) |
| `--debug` | Debug logging |
| `-h`, `--help` | Print usage and exit |

Logging is at debug level from the start.

### Running without hardware

There is no LTC simulator in the tree. The ways to feed the node without a
module are a recorded trace replayed through can_bridge's `trc:` channel (see
`configs/can_bridge/replay.yaml`), a [bag](bag.html) recording, or hand-made
frames from `inspect publish` on `vehicle/can0/rx` with schema `CanFrame`. The
message is multiplexed over two pages, so one frame is never enough: publish a
page-0 frame and a page-1 frame (first data byte `0` and then `1`) and one
sample appears.

## Topics

| Topic | Schema | Contents |
|---|---|---|
| `nodes/motec_ltc/telemetry` | `MotecLtcTelemetry` | Module index, lambda, pump currents `ipn` and `ip` in mA, cell resistance `ri` in ohm, internal temperature in C, heater duty %, battery volts, the sensor state, and seven fault flags: sensor control, internal, sensor wire short, heater failed to heat, open circuit, short to battery, short to ground |

The topic sits under `vehicle/`, not under `nodes/motec_ltc/`, unlike the
other decoders.

The node subscribes to `vehicle/can0/rx`. The one frame it decodes is
`LTC_1_ID1` on id `1120` (`0x460`), multiplexed on its first byte
(`LTC1_Index`): page `0` carries lambda, `Ipn`, internal temperature, the fault
bits and heater duty; page `1` carries the sensor state, battery volts, `Ip`
and `Ri`. The handler fires once both pages have been seen since the last
publish, so every sample carries values from one complete cycle.

`sensorState` is one of `start`, `diagnostics`, `preCal`, `calibration`,
`postCal`, `paused`, `heating`, `running`, `cooling`, `pumpStart`, `pumpOff`.
A raw value outside that table is published as `start`.

### Health

The node publishes `nodes/motec_ltc/health` ([NodeHealth](../libs/node_health.html))
once a second, and at once whenever a check changes:

| Check | Degraded when |
|---|---|
| `can_rx` | no CAN frame of any kind has arrived for a second: the bus is quiet, or the node is subscribed to the wrong key |
| `decoded` | frames are arriving but none of them has been one of this node's messages for two seconds |

`inspect health` prints them. The node exits cleanly on SIGINT or SIGTERM, so a
monitor reads a deliberate stop as `exited` rather than as a crash.

## Services

None.

## Tests

```bash
ctest --test-dir build -L motec_ltc
```

| Target | Labels | Proves |
|---|---|---|
| `motec_ltc_test_messages` | `motec_ltc unit` | Each of the seven fault flags on its own, with the others clear; every sensor state mapped to its own wire value; and the two pump currents kept apart. |

The mapping lives in its own library (`motec_ltc_messages.h`) so it can be tested
without a bus: the generated decoder is checked against cantools and the schema
round-trips itself, but the wiring between the two is only checked here.

## Troubleshooting

**Nothing on `nodes/motec_ltc/telemetry`.** The topic is advertised as soon as the node
starts, so `inspect list` proves only that the node is up. `inspect hz
vehicle/can0/rx` says whether frames arrive at all; none means can_bridge is
not running or the module is on a channel with another topic name, and this
node cannot be redirected. Frames arriving but none on id `1120` means the LTC
is transmitting on a different base identifier, which is set in MoTeC's
configuration tool; only `0x460` is decoded.

**Frames on `1120` arrive and the topic still stays silent.** Both multiplex
pages have to arrive before anything publishes. `inspect echo vehicle/can0/rx
-n 20` and check the first data byte alternates between `0` and `1`; a module
sending only one page never completes a cycle.

**The bus is empty and `vehicle/can/status` shows error counters climbing.**
That is a bit-rate mismatch presenting as silence. can_bridge's shipped config
runs `can0` at 500 kbit/s; the LTC's rate is whatever it was configured with.

**Lambda reads but `sensorState` is never `running`.** That is the module
reporting, not the decoder: read the fault flags in the same sample, which are
what the LTC says about its heater and sensor wiring.
