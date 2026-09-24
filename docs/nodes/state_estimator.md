---
title: state_estimator
parent: Nodes
---

# state_estimator

## Overview

Fuses the MTi-610's strapdown increments, magnetometer and barometer with the
BD992's dual-antenna fixes into one vehicle state: position, attitude, velocity, body acceleration, body
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
points: heading comes from the two antennas or a learned magnetometer, never
from the direction of travel.

With no GNSS at all it still starts, attitude only. Roll and pitch come from
gravity. Heading comes from the magnetometer if an earlier drive learned it, and
it is MAGNETIC until the first fix. `valid` stays false until a position
arrives.

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
| `--calibration-db <file>` | With `--replay` only: the calibration store to seed from and write to. Without it a replay touches none, so an old recording cannot add rows to the car's history. |
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
| `inputs.imu_prefix` | `nodes/mti610/mtdata2` | Subscribed as `<prefix>/**`. Needs `delta_q` and `delta_v` enabled on the MTi; uses `magnetic_field` and `baro_pressure` too if they are. |
| `inputs.gnss_prefix` | `nodes/bd992/gsof` | Subscribed as `<prefix>/**`. Needs GSOF 1, 2, 8, 12, 27 and 38. |
| `outputs.state_key` | `nodes/state_estimator/state` | |
| `outputs.status_key` | `nodes/state_estimator/status` | |
| `outputs.state_decimation` | `1` | Publish one state in N. `1` is every IMU sample. |
| `vehicle.imu_to_body_rpy_deg` | `[180, 0, 0]` | The IMU's orientation in the body frame (x forward, y right, z down). An MTi label-up with x forward is a half turn about x. Learned from here. |
| `vehicle.imu_to_body_sigma_deg` | `[2, 2, 2]` | How well that is known, about the body x, y, z. |
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
| `smoother.keyframe_interval_s` | `0.1` | Keyframes on the IMU's clock when GNSS is not making them. |
| `magnetometer.enabled` | `true` | |
| `magnetometer.sigma` | `0.03` | Per axis, in the MTi's units (about 1 at its calibration field): about 3° of heading. |
| `magnetometer.hard_iron`, `.hard_iron_sigma` | `[0, 0, 0]`, `0.3` | The hard-iron prior. Learned and kept. |
| `magnetometer.soft_iron`, `.soft_iron_sigma` | six zeros, `0.1` | Symmetric soft iron, xx yy zz xy xz yz, in `A = (I + S)/F`. Learned and kept. |
| `magnetometer.*_walk_per_sqrt_h` | `0.01`, `0.005` | How fast hard and soft iron may wander. |
| `magnetometer.gate_sigmas` | `5` | A reading this far from its prediction is a disturbance and is not used. |
| `magnetometer.trust_after_s` | `30` | Seconds learned against the antennas before it gives a heading on its own. |
| `barometer.enabled` | `true` | |
| `barometer.sigma_m` | `0.5` | Per keyframe. |
| `barometer.offset_m`, `.offset_sigma_m`, `.offset_walk_m_per_sqrt_h` | `0`, `300`, `5` | Ellipsoidal height minus ISA pressure altitude: the weather and the geoid. Learned each session, never kept. |
| `barometer.airflow`, `.airflow_sigma`, `.airflow_walk_per_sqrt_h` | `0`, `0.5`, `0.01` | The fraction of dynamic pressure the sensor sees. Learned and kept. |
| `start.anchored` | `true` | Start without GNSS, attitude only. `false` waits for the antennas, as before. |
| `start.anchor_wait_s` | `1.0` | How long without any GNSS before starting anyway. |
| `output.sideslip_min_speed` | `2.0` | m/s. Below it sideslip is undefined and flagged invalid. |
| `calibration.enabled` | `true` | Keep what is learned between sessions. |
| `calibration.database` | `${REDLINE_DATA_DIR}/state_estimator/calibration.sqlite` | Created, with its directory, if absent. |
| `calibration.min_write_interval_s` | `900` | At most one row per group this often (shutdown excepted). |
| `calibration.move_threshold_sigma` | `1.0` | A row when the estimate has moved this far from the last, in its sigmas... |
| `calibration.tighten_ratio` | `0.5` | ...or a sigma has fallen to this fraction of the last row's. |
| `calibration.settle_s` | `120` | Nothing is written this soon after a (re)start. |
| `calibration.load_inflation` | `4.0` | A stored covariance times this is the next session's prior. At least 1. |
| `calibration.moved_sigma` | `4.0` | This far from what was loaded, `health` asks whether something moved. |
| `calibration.segment_s` | `1.0` | One set of installation variables per segment; at most half `smoother.lag_s`. |
| `calibration.*_walk_*` | `0.05` °/√h, `5` mm/√h, `0.05` °/√h | How fast mounting, lever arm and boresight may wander. |

{: .warning }
Every number under `vehicle:` becomes a sideslip error if it is wrong, and a
wrong sideslip angle looks exactly like a right one. The mounting, the lever
arm and the boresight are priors, refined while driving and kept between
sessions, so a centimetre or a couple of degrees off is fine. The reference
point is a definition that nothing the sensors see can check: it must be
measured.

## Topics

In, everything under the two prefixes, dispatched on the schema each sample
carries:

| Key | Schema | Used for |
|---|---|---|
| `nodes/mti610/mtdata2/delta_q` | `XbusDeltaQ` | rotation increment; paired with `delta_v` by packet counter |
| `nodes/mti610/mtdata2/delta_v` | `XbusDeltaV` | velocity increment |
| `nodes/mti610/mtdata2/magnetic_field` | `XbusMagneticField` | magnetometer; a clipped sample is dropped |
| `nodes/mti610/mtdata2/baro_pressure` | `XbusBaroPressure` | barometer, whole Pa |
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
`attitudeValid` can be true without it: before the first GNSS position roll and
pitch are good while position and velocity are placeholders. `headingSource`
says where yaw came from (`dualAntenna`, `magnetometer`, `inertial` since
either, or `none`), and `headingMagnetic` means it is relative to magnetic
north. `gpsTimeValid` is false before any GNSS has been heard; the time is
then the host clock.

`VehicleEstimatorStatus` carries counters (IMU samples bridged and discarded,
GNSS epochs late, timed out or rejected, measurements gated), the last solve's
time, iterations and window size, the current lever-arm, boresight and bias
estimates, the IMU clock offset, the learned mounting as roll/pitch/yaw with
its sigma and what it was learned from (`mountingStraightS`,
`mountingLevelStops`), and the calibration store's state: whether it is open,
which groups this session started from, which have moved since, and how many
rows it has written. It also carries the magnetometer's hard and soft iron
with `magUsed`, `magRejected`, `magLearningS` and `magTrusted`, and the
barometer's offset and airflow with their sigmas and its own height. `anchored`
and `reanchors` describe a start without GNSS.

## Health

`nodes/state_estimator/health` ([NodeHealth](../libs/node_health.html)):

| Check | Not ok when |
|---|---|
| `imu` | nothing under the IMU prefix for 200 ms |
| `gnss` | nothing under the GNSS prefix for 1000 ms |
| `estimate` | degraded while anchored with no GNSS position (it says whether the heading is magnetic), while waiting for a dual-antenna heading, or while the uncertainty is above the valid bounds |
| `solve` | degraded when the last smoother update took 80 ms or more; at 10 Hz keyframes the smoother falls behind the car |
| `calibration` | degraded when the store cannot be opened (the node then runs on the config), when it was found damaged and begun afresh (for the whole session: the old history is in the moved file), or when a learned group is more than `moved_sigma` from what the last session left |

## Calibration kept between sessions

The mounting, lever arm, boresight, magnetometer and barometer airflow the
estimator learns are written to
`calibration.database` as rows, one per group per write, never updated and
never deleted. At start the newest row for each group is loaded as that
group's prior, provided it was learned against the same configured values: each
row carries a hash of them. Re-measure the lever arm and the lever-arm and
boresight rows stop applying while the mounting's still does; change only a
sigma and they all still apply. The history reads directly:

```bash
sqlite3 /data/state_estimator/calibration.sqlite \
  'select datetime(written_at_ns/1e9, "unixepoch"), grp, reason, summary from calibration'
```

A row is written when a group's estimate has moved more than a sigma since the
last row or a sigma has halved, at most every 15 minutes, never in the first
two minutes after a start, and once more at shutdown. The mounting is not
written until something has taught it: seconds of straight, true running, or
stops. The magnetometer is not written until it has been learned against a
dual-antenna heading, nor the airflow until the car has moved. The barometer's
offset is never written: it is the day's weather. No position is ever stored.
The mounting's yaw is learned only on straights, above 8 m/s with the
yaw rate under 1.5°/s and lateral acceleration under 0.5 m/s² for two
seconds; a drift never qualifies. The library side is in
[vehicle_estimator](../libs/vehicle_estimator.html) and
[calibration_store](../libs/calibration_store.html).

## Troubleshooting

**It never initialises.** With GNSS, the estimator waits for a dual-antenna
heading (GSOF 27 with `yawValid`) and will not start without one. Without
GNSS it starts anchored, attitude only. It deliberately does
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

**No heading at a start without GNSS.** The magnetometer gives one only once
it has learned its calibration: `magnetometer.trust_after_s` seconds against
the antennas, this session or in the stored row. A new install, or a
magnetometer prior that was changed in the config, has none. Drive with GNSS
for a minute of turns and the next cold start will have one. `magTrusted` on
the status topic says which.

**Magnetic readings are refused.** `magRejected` climbing means readings are
far from the prediction: steel nearby, a car alongside, or a magnetometer
that has moved or been re-magnetised since its calibration was stored. In the
last case `health` also reports the magnetometer as moved.

**GSOF 27's variances.** They are taken as rad², since the record's angles are
radians on the wire, and the pitch sign is taken as nose-up positive. Neither
is confirmed against a receiver yet.

**Straights read a steady sideslip of a degree or two.** That is the mounting
yaw, and until the car has run a few straights it is the configured one. Check
`mountingStraightS` on the status topic: if it stays at zero, the gate never
held (too slow, or never straight for two seconds), and nothing has been
learned. If it grows and the offset stays, the car really does crab on a
straight, and the mounting has learned the crab.

**`health` says a group "moved?".** The estimate has walked more than
`moved_sigma` from what the previous session stored: an antenna knocked, an
IMU re-seated, a lever arm re-measured without updating the config. The
estimator keeps learning the new value either way. If the change was
deliberate, put the new measurement in the config: its hash will no longer
match the old rows, and the next session starts from the config.

**`health` says the store was damaged.** At start the database failed its
integrity check -- typically a power cut on storage that did not honour a
flush -- and was moved to `calibration.sqlite.corrupt-<time>` beside it, with
its log, and a fresh one begun. The node runs normally and relearns; the old
history is in the moved file, readable with `sqlite3` if it is only partly
damaged. `calibrationStoreRecovered` on the status topic is the same flag.

**No rows are being written.** `calibrationStoreOk` false means the database
could not be opened; the log names the path and the reason, and the node runs
on the config. Otherwise it is too soon: nothing is written in the first
`settle_s` after a start, and a group whose estimate has neither moved nor
tightened since its last row gets no new one.
