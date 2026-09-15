---
title: motec_m1
parent: Nodes
---

# motec_m1

## Overview

`motec_m1` decodes a MoTeC M1 ECU's general CAN broadcast into typed
telemetry, one topic per frame. It reads the raw frames
[can_bridge](can_bridge.html) publishes, decodes them with the generated parser
for `dbcs/motec/Motec_M1_Rev3.dbc`, and publishes 32 topics under
`nodes/motec_m1/`. It needs can_bridge running with the M1 on its `can0`
channel. It does not talk to the ECU, and it publishes a subset of the DBC's
signals, the ones the schema comments call "key signals for dashboard use".

## Running it

The node takes only command-line options, and the only one is `--help`. The
frame topic and every output topic are fixed in the source.

```bash
./build/nodes/motec_m1/motec_m1
```

| Option | Meaning |
|---|---|
| `-s`, `--source` | Zenoh key carrying CAN frames (default `vehicle/can0/rx`) |
| `-p`, `--prefix` | Key prefix for this node's topics (default `nodes/motec_m1`) |
| `--debug` | Debug logging |
| `-h`, `--help` | Print usage and exit |

Logging is at debug level from the start.

### Running without hardware

There is no M1 simulator in the tree. Replay a recorded trace through
can_bridge's `trc:` channel (see `configs/can_bridge/replay.yaml`) or a
[bag](bag.html) recording, or hand-make frames with `inspect publish`. None of
the M1 messages is multiplexed, so a single frame is enough for any topic that
comes from one id:

```bash
./build/nodes/inspect/inspect echo nodes/motec_m1/engine_air -n 1 &
# 3000 rpm, 100.0 kPa, 25.0 C, 50.0 % throttle, big-endian
./build/nodes/inspect/inspect publish vehicle/can0/rx --schema CanFrame \
    --data '{"id":1600,"len":8,"data":[11,184,3,232,0,250,1,244]}'
```

## Topics

Every topic is under `nodes/motec_m1/`. The M1 transmits at base `0x640`
(`1600`) with the auxiliary blocks at `0x6A0`; the ids below are what the DBC
names and are decimal in `CanFrame`.

| Topic | Schema | Id | Contents |
|---|---|---|---|
| `engine_air` | `MotecM1EngineAir` | `0x640` | Engine speed, MAP, inlet manifold temperature, throttle position |
| `fuel_status` | `MotecM1FuelStatus` | `0x641` | Fuel volume, mixture aim, fuel pressure, injector duty, engine efficiency |
| `throttle_timing` | `MotecM1ThrottleTiming` | `0x642` | Pedal, engine load, ignition and fuel timing |
| `cuts_oil_pressure` | `MotecM1CutsAndOilPressure` | `0x644` | Ignition and fuel cut counts and averages, cylinder 1 pulse width, cut and timing states, oil pressure |
| `boost_status` | `MotecM1BoostStatus` | `0x645` | Boost pressure and aim, actuator duty, gear lever force |
| `inlet_cam`, `exhaust_cam` | `MotecM1InletCam`, `MotecM1ExhaustCam` | `0x646`, `0x647` | Cam aim, both banks' position and duty |
| `wheel_speeds` | `MotecM1WheelSpeeds` | `0x648` | Four wheel speeds in km/h |
| `temperatures` | `MotecM1Temperatures` | `0x649` | Coolant, oil, fuel, ambient and airbox temperature, ECU battery volts, fuel used |
| `environment` | `MotecM1Environment` | `0x64A` | Exhaust temperature, average load, ignition rev limit, ambient pressure |
| `runtime_warnings` | `MotecM1RuntimeWarnings` | `0x64C` | Run time, ECU up time, warning source and eight warning flags |
| `states` | `MotecM1States` | `0x64D` | Fuel pump, engine, launch, anti-lag, limiter, boost, cut, overrun, knock, purge, closed-loop and throttle states, gear |
| `diagnostics` | `MotecM1Diagnostics` | `0x64E` | Launch, anti-lag, boost and closed-loop diagnostics, and the switch inputs |
| `totals` | `MotecM1Totals` | `0x64F` | Run hours, closed-loop trims, gearbox temperature, tank level |
| `driver_controls` | `MotecM1DriverControls` | `0x650` | Rotaries 1..6, switches 1..8 |
| `exhaust` | `MotecM1Exhaust` | `0x651` | Lambda overall and per bank, exhaust temperature per bank |
| `fuel_secondary` | `MotecM1FuelSecondary` | `0x652` | Secondary injector contribution, timing and duty |
| `pressures` | `MotecM1Pressures` | `0x655` | Brake front and rear, coolant, power steering |
| `flows` | `MotecM1Flows` | `0x656` | Steering angle, inlet and airbox mass flow, fuel flow |
| `injector_pressures` | `MotecM1InjectorPressures` | `0x657` | Primary and secondary injector pressure, gearbox shaft speeds |
| `vehicle_dynamics` | `MotecM1VehicleDynamics` | `0x658` | Lateral, longitudinal and vertical g, yaw rate |
| `lap_timing` | `MotecM1LapTiming` | `0x65B` | Lap time, running lap time, lap number and distance |
| `diff_rotary` | `MotecM1DiffAndRotary` | `0x65C` | Front differential temperature, rotaries 7 and 8 |
| `brake_temperatures` | `MotecM1BrakeTemperatures` | `0x65D` | Four brake temperatures |
| `exhaust_pressures` | `MotecM1ExhaustPressures` | `0x65E` | Exhaust pressure per bank, crankcase pressure, alternator current |
| `thresholds_limits` | `MotecM1ThresholdsAndLimits` | `0x65F` | Knock threshold, logging used, pit speed limit |
| `aux_outputs`, `aux_output5` | `MotecM1AuxOutputs`, `MotecM1AuxOutput5` | `0x6A0`, `0x6A1` | Auxiliary output duties 1..4, and 5 |

{: .note }
The five temperatures in `temperatures` are Int16 as of 2026-09-15. They were
Int8, which wrapped anything above 127 C negative, so a recording made before
then misreads those fields.

Four topics are built from a pair of frames and publish only when both have
arrived since the last publish:

| Topic | Schema | Ids | Contents |
|---|---|---|---|
| `knock_levels_1_12` | `MotecM1KnockLevels1to12` | `0x643` + `0x659` | Knock level for cylinders 1..12 |
| `ignition_trim_1_12` | `MotecM1IgnitionTrim1to12` | `0x64B` + `0x65A` | Ignition trim in degrees for cylinders 1..12 |
| `fuel_direct_all` | `MotecM1FuelDirectAll` | `0x653` + `0x654` | Direct injection fuel pressure loop, both banks |
| `turbo` | `MotecM1Turbo` | `0x6A6` + `0x6A7` | Turbo speed, inlet and outlet temperature and inlet pressure, both banks |

The node subscribes to `vehicle/can0/rx` with schema `CanFrame`. A frame
shorter than eight bytes is rejected rather than decoded with padding.

### Health

The node publishes `nodes/motec_m1/health` ([NodeHealth](../libs/node_health.html))
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
ctest --test-dir build -L motec_m1
```

| Target | Labels | Proves |
|---|---|---|
| `motec_m1_test_messages` | `motec_m1 unit` | Which signal becomes which field, for one message of each shape and for all four messages assembled from two frames -- where a field taken from the wrong frame is otherwise invisible. |

The mapping lives in its own library (`motec_m1_messages.h`) so it can be tested
without a bus: the generated decoder is checked against cantools and the schema
round-trips itself, but the wiring between the two is only checked here.

## Troubleshooting

**All 32 topics are advertised and none publishes.** Advertisement happens at
startup, so `inspect list` only says the node is up. `inspect hz
vehicle/can0/rx` says whether frames arrive; none means can_bridge is not
running, or the M1 is on a channel whose topic is not `vehicle/can0/rx`, and
this node cannot be redirected. Frames arriving but none between `1600` and
`1703` means the M1's CAN transmit template is at another base address, or the
general broadcast is not enabled in its package; only base `0x640` is decoded.

**Most topics publish, but one of the paired ones never does.** The pair
waits for both of its ids. An M1 package that transmits `0x643` but not
`0x659`, say, gives no `knock_levels_1_12` at all rather than half of one.
`inspect echo vehicle/can0/rx -n 200` and check both ids appear.

**The bus is empty and `vehicle/can/status` shows error counters climbing or
the channel bus-off.** That is a bit-rate mismatch presenting as silence.
can_bridge's shipped config runs `can0` at 500 kbit/s; the M1's rate is set in
its package, and 1 Mbit/s is the common MoTeC choice.

**A topic publishes at the wrong rate for a dashboard.** Each topic publishes
once per frame, at whatever rate the M1 transmits that id; the node neither
throttles nor resamples. Change the transmit rate in the M1 package, or let the
widget decide how often to redraw.
