---
title: megasquirt
parent: Nodes
---

# megasquirt

## Overview

`megasquirt` turns a Megasquirt ECU's simplified dash broadcast into one typed
telemetry message. It subscribes to the raw frames [can_bridge](can_bridge.html)
publishes, decodes the five dash frames `dash0` to `dash4` with the generated
parser for `dbcs/megasquirt/Megasquirt_simplified_dash_broadcast.dbc`, and
publishes a `MegasquirtDash` each time it has a complete set. It needs
can_bridge running with the ECU on its `can0` channel and nothing else; it does
not talk to the ECU, and it does not decode the full realtime broadcast that the
other DBC in that directory describes.

## Running it

The node takes only command-line options, and the only one is `--help`. The
frame topic and the output topic are fixed in the source.

```bash
./build/nodes/megasquirt/megasquirt
```

| Option | Meaning |
|---|---|
| `-h`, `--help` | Print usage and exit |

Logging is at debug level from the start; there is no flag to quieten it.

### Running without hardware

`megasquirt_test_frames`, built alongside the node, publishes a full cycle of
the five dash frames about twenty times a second with slowly varying values, on
the same topic the node reads.

```bash
./build/nodes/megasquirt/megasquirt_test_frames          # -k to use another key
./build/nodes/megasquirt/megasquirt &
./build/nodes/inspect/inspect echo nodes/megasquirt/dash -n 3
```

can_bridge does not need to be running for this: the publisher writes straight
to `vehicle/can0/rx`. A recorded trace replayed through can_bridge's `trc:`
channel (see `configs/can_bridge/replay.yaml`) or through [bag](bag.html) works
the same way.

## Topics

| Topic | Schema | Contents |
|---|---|---|
| `nodes/megasquirt/dash` | `MegasquirtDash` | RPM, MAP in kPa, TPS %, coolant and intake air temperature in F, ignition advance, injector pulse widths in ms, EGT in F, EGO correction %, AFR and AFR target, knock retard, two generic sensor inputs, battery volts, launch and traction control retard, vehicle speed in m/s |

The node subscribes to `vehicle/can0/rx` with schema `CanFrame`. The frames it
decodes are the five the DBC names:

| CAN id | Message | Carries |
|---|---|---|
| `1512` (`0x5E8`) | `megasquirt_dash0` | `map`, `rpm`, `clt`, `tps` |
| `1513` (`0x5E9`) | `megasquirt_dash1` | `pw1`, `pw2`, `mat`, `adv_deg` |
| `1514` (`0x5EA`) | `megasquirt_dash2` | `afrtgt1`, `AFR1`, `egocor1`, `egt1`, `pwseq1` |
| `1515` (`0x5EB`) | `megasquirt_dash3` | `batt`, `sensors1`, `sensors2`, `knk_rtd` |
| `1516` (`0x5EC`) | `megasquirt_dash4` | `VSS1`, `tc_retard`, `launch_timing` |

The five are registered as one aggregate: the message goes out once every one
of them has arrived since the last publish, then the set resets. A frame
shorter than eight bytes is rejected rather than decoded with padding.

### Health

The node publishes `nodes/megasquirt/health` ([NodeHealth](../libs/node_health.html))
once a second, and at once whenever a check changes:

| Check | Degraded when |
|---|---|
| `can_rx` | no CAN frame of any kind has arrived for a second: the bus is quiet, or the node is subscribed to the wrong key |
| `decoded` | frames are arriving but none of them has been one of this node's messages for two seconds |

`inspect health` prints them. The node exits cleanly on SIGINT or SIGTERM, so a
monitor reads a deliberate stop as `exited` rather than as a crash.

## Services

None.

## Troubleshooting

**Nothing on `nodes/megasquirt/dash`, and the node logs nothing.** Start with
`inspect hz vehicle/can0/rx`. No frames at all means can_bridge is not running,
or the ECU is wired to a channel whose topic is not `vehicle/can0/rx`; this
node cannot be pointed elsewhere, so the fix is in can_bridge's config. Frames
arriving but none with ids `1512` to `1516` means the ECU's dash broadcast base
is set to something other than `0x5E8` in the tune, or the simplified dash
broadcast is not enabled at all.

**Four of the five ids arrive and the topic stays silent.** The aggregate waits
for a complete set, so a tune that leaves one dash message disabled produces no
telemetry rather than partial telemetry. `inspect echo vehicle/can0/rx` and
count the distinct ids.

**The bus is empty and `vehicle/can/status` shows the error counters rising or
the channel going bus-off.** That is a bit-rate mismatch between can_bridge's
channel and the ECU, which presents as silence rather than as garbage.
can_bridge's shipped config runs `can0` at 500 kbit/s; the ECU's rate is set in
its tune.
