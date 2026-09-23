---
title: state_estimator
parent: Nodes
---

# state_estimator

## Overview

Fuses the MTi-610's strapdown increments with the BD992's dual-antenna fixes
into one vehicle state: position, attitude, velocity, body acceleration, body
rate and sideslip, at the IMU's 100 Hz. It publishes nothing it has not
estimated. There is no hardware of its own; it subscribes to what
[mti610_bridge](mti610_bridge.html) and [bd992_bridge](bd992_bridge.html) put
on the bus.

The estimator is a fixed-lag smoother over an IMU-preintegration factor graph,
with the lever arm to the primary antenna and the antenna boresight estimated
as it drives. The node is thin: it decodes capnp, pairs records into samples
and epochs, and hands them to [vehicle_estimator](../libs/vehicle_estimator.html),
which has no zenoh or capnp in it. Why it is shaped this way, and what it
cannot observe, is in the [design note](../design/state-estimation.html).

It is built for a car that slides. Nothing assumes the car goes where it
points: heading comes from the two antennas, never from the direction of
travel.

## Running it

```bash
./build/nodes/state_estimator/state_estimator --config configs/state_estimator/state_estimator.yaml --check
./build/nodes/state_estimator/state_estimator --config configs/state_estimator/state_estimator.yaml
./build/nodes/state_estimator/state_estimator --replay /path/to/bag
```

| Option | |
| --- | --- |
| `--config <file>` | The YAML below. Defaults to `configs/state_estimator/state_estimator.yaml`. |
| `--check` | Parse and validate the config, print the input and output keys, and exit. |
| `--replay <bag>` | Run the same pipeline over a recorded bag directory instead of the bus, as fast as it goes, and publish what it computes. Exits 1 if the estimator never initialised. |
| `--debug` | Verbose logging. |

Exit codes are 0, 1 for a failure (a bad config, an unreadable bag, a replay
that never initialised) and 2 for bad usage.

`--replay` is for pointing scope at a live-looking output from a recording.
To compare the fixed-lag output with a whole-drive solve, use
[estimator_offline](../tools/estimator_offline.html), which writes both into
a new bag.

## Configuration

`configs/state_estimator/state_estimator.yaml`:

| Key | Default | |
| --- | --- | --- |
| `inputs.imu_prefix` | `nodes/mti610/mtdata2` | Subscribed as `<prefix>/**`. Needs `delta_q` and `delta_v` enabled on the MTi. |
| `inputs.gnss_prefix` | `nodes/bd992/gsof` | Subscribed as `<prefix>/**`. Needs GSOF 1, 2, 8, 12, 27 and 38. |
| `outputs.state_key` | `nodes/state_estimator/state` | |
| `outputs.status_key` | `nodes/state_estimator/status` | |
| `outputs.state_decimation` | `1` | Publish one state in N. `1` is every IMU sample. |
| `vehicle.imu_to_body_rpy_deg` | `[180, 0, 0]` | The IMU's orientation in the body frame (x forward, y right, z down). An MTi label-up with x forward is a half turn about x. |
| `vehicle.reference_point_m` | `[0, 0, 0]` | Where to report position and sideslip (the CG, or the rear axle), IMU frame. |
| `vehicle.lever_arm_m` | `[0, 0, 1.2]` | IMU origin to the primary antenna's phase centre, IMU frame. Estimated from here. |
| `vehicle.lever_arm_sigma_m` | `0.02` | How well it was measured, per axis. |
| `vehicle.antenna2_lever_arm_m` | `[-1.2, 0, 1.2]` | IMU origin to the secondary antenna. Only its direction from the primary matters. |
| `vehicle.boresight_sigma_deg` | `1.0` | How well that direction is known. |
| `imu.dv_frame` | `end` | Which body frame `delta_v` is expressed in; see Troubleshooting. |
| `imu.gyro_noise_density_dps` | `0.007` | deg/s/√Hz, MTi-610 datasheet. |
| `imu.accel_noise_density_mps2` | `0.00059` | m/s²/√Hz (60 µg/√Hz). |
| `imu.gyro_bias_walk` | `2.0e-5` | rad/s/√s. |
| `imu.accel_bias_walk` | `2.0e-4` | m/s²/√s. |
| `imu.time_offset_s` | `0.0` | GNSS latency floor minus IMU latency floor; see Troubleshooting. |
| `gnss.burst_ms` | `15` | Records arriving within this of the first form one epoch. |
| `gnss.velocity_sigma_horizontal` | `0.03` | m/s. GSOF 8 carries no sigma. |
| `gnss.velocity_sigma_vertical` | `0.06` | m/s. |
| `gnss.default_sigma_horizontal` | `2.0` | m, used when no GSOF 12 is fresh. |
| `gnss.default_sigma_vertical` | `4.0` | m. |
| `smoother.lag_s` | `3.0` | Keyframes older than this are marginalised. |
| `smoother.max_iterations` | `8` | Per keyframe. |
| `smoother.time_budget_ms` | `40` | |
| `output.sideslip_min_speed` | `2.0` | m/s. Below it sideslip is undefined and flagged invalid. |

{: .warning }
Every number under `vehicle:` becomes a sideslip error if it is wrong, and a
wrong sideslip angle looks exactly like a right one. The lever arm and the
boresight are refined while driving, so a centimetre or a degree off is fine.
The IMU's mounting rotation and the reference point are not estimated at all,
because nothing the sensors see can tell them apart from the car's own
motion; they must be measured.

## Topics

In, everything under the two prefixes, dispatched on the schema each sample
carries:

| Key | Schema | Used for |
|---|---|---|
| `nodes/mti610/mtdata2/delta_q` | `XbusDeltaQ` | rotation increment; paired with `delta_v` by packet counter |
| `nodes/mti610/mtdata2/delta_v` | `XbusDeltaV` | velocity increment |
| `nodes/bd992/gsof/position_time` | `GsofPositionTime` | GSOF 1: the GPS time of the epoch |
| `nodes/bd992/gsof/current_time_utc` | `GsofCurrentTimeUtc` | GSOF 16: the week, when no GSOF 1 has been seen |
| `nodes/bd992/gsof/lat_long_height` | `GsofLatLongHeight` | GSOF 2: position |
| `nodes/bd992/gsof/velocity` | `GsofVelocity` | GSOF 8: velocity |
| `nodes/bd992/gsof/position_sigma` | `GsofPositionSigma` | GSOF 12: position accuracy, used by age |
| `nodes/bd992/gsof/attitude_info` | `GsofAttitudeInfo` | GSOF 27: dual-antenna yaw and pitch |
| `nodes/bd992/gsof/position_type` | `GsofPositionType` | GSOF 38: fix type, used by age |

Anything else under the prefixes is ignored.

Out:

| Key | Schema | Rate |
|---|---|---|
| `nodes/state_estimator/state` | `VehicleState` | every IMU sample / `state_decimation` |
| `nodes/state_estimator/status` | `VehicleEstimatorStatus` | 1 Hz |

`VehicleState` is flat scalars at the reference point: GPS time, latitude,
longitude and ellipsoidal height, roll/pitch/yaw, NED velocity, body (FRD)
velocity, acceleration and rate, `speedMps`, `sideslipDeg` with
`sideslipValid`, one-sigma per group, and the fix type. Sideslip is
`atan2(v_right, v_forward)` at the reference point, positive to the right.
Check `valid` before using anything: it is false until the estimator has
initialised and while its uncertainty is above the configured bounds.

`VehicleEstimatorStatus` carries counters (IMU samples bridged and discarded,
GNSS epochs late, timed out or rejected, measurements gated), the last solve's
time, iterations and window size, the current lever-arm, boresight and bias
estimates, and the IMU clock offset.

## Health

`nodes/state_estimator/health` ([NodeHealth](../libs/node_health.html)):

| Check | Not ok when |
|---|---|
| `imu` | nothing under the IMU prefix for 200 ms |
| `gnss` | nothing under the GNSS prefix for 1000 ms |
| `estimate` | degraded while waiting for a dual-antenna heading, or while the uncertainty is above the valid bounds |
| `solve` | degraded when the last smoother update took 80 ms or more; at 10 Hz keyframes the smoother falls behind the car |

## Troubleshooting

**It never initialises.** The estimator waits for a dual-antenna heading
(GSOF 27 with `yawValid`) and will not start without one. It deliberately does
not fall back to the direction of travel: while drifting, course is not
heading, and a filter started on course starts with its heading off by the
slip angle. Check the receiver is sending GSOF 27 and that its antennas are
calibrated. `health` says `waiting for a dual-antenna heading` in this state.

**Everything is slightly wrong in corners.** Suspect `imu.time_offset_s`.
The two sensors have no shared clock yet, so each stream is mapped onto the
host clock through the minimum of its arrival latency. That lines the two up
except for the difference between their latency floors, which arrival times
cannot see, and a residual skew shows as position and attitude error that
grows with speed and yaw rate (10 ms at 20 m/s is 0.2 m). Calibrate it here
until the two are wired to a common PPS. `estimator_sim` prints the value its
recordings need.

**Epochs are counted as undated.** GNSS records arrive on separate topics, one
per GSOF record. The node groups everything that arrives within `burst_ms` of
the first record into one epoch; a second record of a type the epoch already
holds closes it early. The epoch takes its time from GSOF 1, or failing that
from GSOF 27's own time of week with the week from the last GSOF 1 or 16. An
epoch with neither is dropped and counted, never guessed. GSOF 12 and 38
arrive at 1 Hz and are used by age rather than by epoch. If the receiver is
not sending GSOF 1, every position epoch is dropped.

{: .note }
`bd992_bridge` was left alone for this. Stamping each GSOF sample with a
transmission number and GPS time in the zenoh attachment would have made
pairing exact, but an attachment does not survive `bag record` and `bag play`,
and it would break the 8-byte schema-fingerprint check that rides in the same
attachment. Arrival-age pairing is the rule `map_match` already follows.

**The attitude is upside down, or turns the wrong way.** The MTi's axis
convention and the frame `delta_v` is expressed in (`imu.dv_frame: end`) come
from the datasheet and reference manual and are not yet confirmed on a device
(2026-09-23). Neither can be caught in simulation, because the simulator
shares the reading. Before trusting a real drive, check gravity lands on the
body's +z (down) while parked, and that a left turn makes yaw decrease in step
with GSOF 27.

**GSOF 27's variances.** They are taken as rad², since the record's angles are
radians on the wire, and the pitch sign is taken as nose-up positive. Neither
is confirmed against a receiver yet.
