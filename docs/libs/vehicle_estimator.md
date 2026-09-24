---
title: vehicle_estimator
parent: Libraries
---

# vehicle_estimator

## Overview

The vehicle state estimator fuses an MTi-610's strapdown increments with a
dual-antenna BD992's fixes in a fixed-lag smoother on ECEF states. At IMU rate
it produces position, attitude, velocity, acceleration, angular rate and
sideslip. Each GNSS epoch becomes a keyframe (attitude, position, velocity
and both IMU biases), and when GNSS is absent the IMU's clock makes one every
0.1 s instead. The MTi's magnetometer and barometer add a weak heading and a
height to any keyframe. The installation is learned alongside: how the IMU is
mounted in the body, the IMU-to-antenna lever arm, the antenna baseline's
boresight, the magnetometer's hard and soft iron and the barometer's offset
and airflow, each from its configured prior along a slow random walk. Between
keyframes the newest one is carried forward through the IMU samples, so the
output rate is the IMU's.

The library is pure computation, with no zenoh and no capnp. The
[state_estimator](../nodes/state_estimator.html) node and
[estimator_offline](../tools/estimator_offline.html) decode into
`measurements.h` and call the same object, and so do the tests. The graph
machinery is [factor_graph](factor_graph.html) and the IMU model is
[imu_preint](imu_preint.html). The reasoning behind the design is in the
[state estimation design note](../design/state-estimation.html).

## Public headers

| Header | |
| --- | --- |
| `vehicle_estimator/measurements.h` | `ImuSample` (with an optional magnetometer vector and pressure), `FixQuality`, `GnssPosition`, `GnssVelocity`, `DualAntenna`, `GnssEpoch` in; `VehicleState` and `HeadingSource` out. |
| `vehicle_estimator/config.h` | `EstimatorConfig`: noise, geometry, gating, lag, start-up thresholds. Defaults describe an MTi-610 mounted x-forward, z-up and a BD992 on RTX. |
| `vehicle_estimator/clock.h` | `ClockOffset` and `TimeMapper`: both sensors on GPS time from host arrival times. |
| `vehicle_estimator/factors.h` | `gnssPosition`, `gnssVelocity`, `dualAntenna`, `straightDriving`, `stationaryLevel`, `zeroVelocity`, `baroHeight`, `magnetometer`, `magneticHeading`, `calibrationPrior`, `calibrationWalk`, the priors, `Baseline`, `sqrtInformation`. |
| `vehicle_estimator/magnetic_reference.h` | `magneticField()`: WMM-HR 2025 at a position and GPS time, as NED, intensity, inclination and declination; `MagneticReference` caches it for 1 km or 1 h. |
| `vehicle_estimator/atmosphere.h` | The ISA troposphere: `isa::altitude`, `isa::pressure`, `isa::density`. |
| `vehicle_estimator/calibration.h` | `CalibrationSet` and the segment keys; the per-group policy for keeping it between sessions: `priorHash`, `decideWrite`, `loadPrior`, `distanceFrom`, `extract`/`apply`, `summary`. No I/O. |
| `vehicle_estimator/estimator.h` | `Estimator`, `EstimatorStatus`, `KeyframeRecord`. |
| `vehicle_estimator/offline.h` | `OfflineSmoother`: every keyframe of a drive solved together. |
| `vehicle_estimator/sim/scenario.h` | (target `vehicle_estimator_sim`) `skidpad`, `figureEight`, `parked`, `track`, `stopAndGo`, `scripted`, `SensorModel` (with a `MountStep` knock), `Scenario`. |

## Using it

Link the CMake target `vehicle_estimator`. Link `vehicle_estimator_sim` as
well for the simulator, which is a library rather than test code because
`tools/estimator_sim` writes its streams into a bag.

```cpp
vehicle_estimator::Estimator est(config);
est.addImu(sample);        // raw MTi sample: sequenced, bridged, mapped to GPS time
est.addGnss(epoch);        // one receiver transmission
est.process();             // every epoch the IMU now covers becomes a keyframe
if (auto s = est.latest()) publish(*s);
```

`addImuIncrement()` takes an increment already on GPS time, for a
synchronised source or a test. For a whole-drive solution, pass
`setKeyframeSink()` a lambda that feeds `OfflineSmoother::add()`, replay the
drive, then call `solve()`.

## Behaviour worth knowing

**Frames.** `e` is ECEF, `n` is local NED at the reference point, and `b` is
the vehicle body in SAE J670 (x forward, y right, z down). `R_b_i` rotates
the IMU frame into the body; the default is a half turn about x.
`VehicleState::sideslip` is `atan2(v_y, v_x)` of the velocity at
`reference_point` in the body frame, positive to the right. It is flagged
invalid below `sideslip_min_speed`. An IMU-to-body yaw error goes straight
into sideslip, one for one, which is why `R_b_i` is learned (below) and why its
uncertainty is part of the reported attitude and sideslip sigmas. It is not
part of `valid`, which judges the navigation solution only.

**The installation is a random walk, not a constant.** Mounting, lever arm and
boresight get a new set of variables every `calibration_segment` (1 s), joined
by a walk factor at `mounting_walk`, `lever_arm_walk` and `boresight_walk`
(0.05°/√h, 5 mm/√h, 0.05°/√h). A constant could only ever become more
certain, so after a long session it would be overconfident, would absorb every
small model error, and could not follow an antenna that was knocked. With the
walk its uncertainty settles instead. `OfflineSmoother::Result::calibration`
gives the whole drive's segments, smoothed, which is the calibration's history.
Segments are stamped when they open, so `calibration_segment` must be at most
half the lag.

**The mounting is learned from an assumption, and a drift car breaks it.**
IMU and GNSS agree with any mounting, so two pseudo-measurements supply it.
Parked, the body is level to within the road's grade (σ 1.5°): over many stops
facing different ways, that gives roll and pitch. Running straight and true,
the body neither slides nor heaves (σ 0.5°): that gives yaw and pitch. The
second is an assumption about the car, so its gate is strict and must hold for
2 s: above 8 m/s, yaw rate under 1.5°/s, lateral acceleration under
0.5 m/s², with at most one factor a second because the error (crosswind,
crown, toe) is correlated. `EstimatorStatus::mount_straight_s` and
`mount_level_stops` count what it was learned from. A car that crabs on a
straight will teach the mounting its crab.

**Heading comes from the antennas or a learned magnetometer, never from the
track.** A drifting car's course over ground is not its heading. With GNSS,
the initialiser waits for a dual-antenna yaw and builds the attitude by TRIAD
from the baseline and the specific force. It levels on the accelerometer after
subtracting the acceleration implied by successive GNSS velocities, so a start
on the move is level too.

**Keyframes on the IMU's clock.** When no GNSS epoch is due, a keyframe is made
every `keyframe_interval` (0.1 s) on a GPS-time grid, once the IMU is
`max_gnss_wait` past it, so a late epoch is never pre-empted. An outage
therefore has keyframes: the IMU buffer stays bounded, the sigmas grow, and
the magnetometer, barometer and zero-velocity factors have somewhere to go. A
keyframe is judged parked when the IMU has been still for 1 s AND the
estimated speed is under 0.1 m/s: an IMU alone cannot tell parked from
cruising straight, and a zero-velocity factor during a slow roll-out once
taught the lever arm 3.6 cm of error.

**The magnetometer is a weak heading, learned against a strong one.** The
model is `m = (I + S)/F · R_e_iᵀ · B_e + h`, with `B_e` from WMM-HR at the
current position, a 3-vector hard iron `h` and a symmetric soft iron `S` in
the MTi's arbitrary units. While the antennas give a heading, turns teach the
horizontal hard and soft iron (`EstimatorStatus::mag_learning_s` counts that
time). The vertical terms stay near their priors, because a car barely rolls.
Each reading is gated at `mag_gate_sigmas` on its full predicted covariance,
the calibration's included, so a large unlearned hard iron is still accepted
while a bridge or a car alongside is refused (`mag_rejected`). It pays where
nothing else gives heading: parked through an outage (0.21° against 3.9°
without it over five minutes) and at a cold start. Driving, GNSS velocity
through the corners already carries the heading and it adds nothing.

**The barometer is a height, with an offset learned every session.** The
factor is `H_ISA(p − c·½ρ|v|²) + b = h`. The offset `b` absorbs the weather,
the geoid and the ISA's mismatch; it starts each session from its config
prior (0 ± 300 m) and is never stored, because yesterday's weather is not
today's. The airflow `c` is the fraction of dynamic pressure the sensor sees
where it is mounted, and is kept. Through a 60 s outage on hills it held
height to 0.24 m, against 0.78 m on the IMU alone.

**A start with no GNSS runs the same graph, anchored.** With no receiver heard
at all after `anchor_wait` (1 s), there is no GPS time and no position. The IMU
goes onto the host clock, and the graph starts at a placeholder (45° N, 0° E)
with roll and pitch from gravity. Heading comes from the magnetometer, relative
to MAGNETIC north, and only if its calibration was learned: `mag_trust_after`
seconds of it this session, or a stored row that had as much. Otherwise there
is no heading. The output is attitude only: `valid` false, `attitude_valid`
true, `heading_magnetic` and `heading_source` saying where yaw came from, and
`gps_time_valid` false while stamps are on the host clock.

When the receiver first appears, the anchored attitude and biases carry over
onto GPS time. At the first position they are rotated into the true frame
once, declination included (`EstimatorStatus::reanchors`), and everything
after is an ordinary start. A start with no heading instead waits for the
antennas. The anchor's position prior is tight (1 cm), because the anchor is
the frame's origin. A loose one also broke the covariance: against the IMU
chain's stiffness a 10 m prior is below what a double-precision factorisation
can resolve, and the window had no covariance at all.

**Time alignment is naive, and it has one calibration.** The minimum of
`host − device` over a window is the clock offset plus that stream's latency
floor. `TimeMapper` maps IMU time to GPS time through both minima and slews
at most 2 ms/s, so IMU stamps stay monotonic. The one thing it cannot see is
the difference between the two latency floors. That is
`EstimatorConfig::imu_time_offset`, and it has to be set for the
installation. `estimator_sim` prints the value its recording needs. A shared
PPS or SyncIn can replace this later behind the same interface.

**Gating has a lockout breaker.** An innovation beyond `gate_sigmas` is not
used. However, `gate_lockout` rejections in a row mean the prediction is the
thing that is wrong. The next measurement then goes in under the robust loss,
and the count restarts. Every measurement residual passes through a
pseudo-Huber loss at `robust_delta` sigmas.

**A reset loses the state, not the calibration.** A gap the IMU cannot bridge
restarts the smoother. The installation as the newest segment had it, with its
full covariance, becomes the next start's prior. `seedCalibration()` supplies
one from an earlier session instead; a carried set wins over a seeded one.
`KeyframeRecord::start` marks that prior, and the offline batch replaces it
with a walk factor from the last segment it has, so nothing is counted twice.

**No claim without a covariance.** If the window's covariance cannot be
computed, the sigmas read zero. `attitude_valid` and `valid` then stay false
rather than pass their bounds on a zero. Every scenario test asserts this.
In a long outage the covariance comes from a square-root factorisation
instead (see [factor_graph](factor_graph.html)), and a ten-minute outage keeps
a covariance on every keyframe.

**Gravity comes from the caller.** `EstimatorConfig::gravity` is a
`geodesy::GravityModel`; null means normal gravity. The node hands in
[deflec](deflec.html)'s `DeflectedGravity`, which leans gravity by NGS's
DEFLEC2022 over North America. `EstimatorStatus::gravity_deflection` says
whether the newest keyframe's gravity was more than normal gravity. In the Colorado foothills the plumb line leans
29.4" east, and ignoring it leaves the estimated attitude leaning by that
much; modelled, the attitude error is the same as in a world with no
deflection at all. Through an outage the same arcseconds are real but small
next to what a straight road does to an unaided MTi's heading.

**Keeping it between sessions is a policy here and I/O elsewhere.**
`calibration.h` decides, per group, what the node's
[calibration_store](calibration_store.html) keeps. `priorHash` is FNV-1a over
the group's configured means (the boresight's covers both antennas); sigmas are
left out, so changing your confidence keeps what was learned. `decideWrite`
writes when the estimate has moved more than 1σ of the last row, or a sigma has
halved, at most every 15 minutes, and not in the first two minutes after a
(re)start. `loadPrior` widens a stored covariance by 4, floors it, and caps it
at the config's sigma. The groups are the mounting, lever arm, boresight,
magnetometer and barometer airflow. The mounting, magnetometer and airflow
write nothing until something has taught them: straights and stops, a
dual-antenna heading, and speed respectively.

**Late GNSS gets a window.** An epoch becomes a keyframe only once the IMU is
`gnss_reorder_window` past it, so an epoch that arrives after its successor
still gets its turn. An epoch the IMU never covers is dropped after
`max_gnss_wait`.

**Unverified on hardware, as of 2026-09-24:**

- the MTi's `dv` frame (`DvFrame::end` is the default)
- the magnetometer's axes against the IMU's, and its clipping flag
- the sign and size of the barometer's airflow term where it is mounted
- the MTi axis convention behind `R_b_i`
- that GSOF 27 variances are in rad²
- the sign of GSOF 27 pitch

A static gravity check and a yaw-rotation sign check on a real bag should
come before trusting a drive.

## Tests

Every test runs on simulated drives from `vehicle_estimator_sim`, scored
against exact truth.

| Target | Label | What it proves |
| --- | --- | --- |
| `vehicle_estimator_test_scenarios` | unit | A skidpad swinging 3° to 26° of slip, a figure of eight whose slip changes sign, a spin past 90°, and a parked car. |
| `vehicle_estimator_test_calibration` | unit | Biases, lever arm and boresight recovered while turning. What cannot be learned stays at its prior. |
| `vehicle_estimator_test_robustness` | unit | A GNSS outage, a multipath jump, dropped IMU samples, RTX falling to autonomous, out-of-order epochs, a wrong or missing start-up heading, NaN measurements. |
| `vehicle_estimator_test_clock` | unit | The latency envelope, its window, the settle period, the slew limit. |
| `vehicle_estimator_test_factors` | unit | Each factor is zero at the truth, has finite-difference Jacobians, bends under the robust loss, wraps yaw, and includes the lever arm's rotational velocity. The calibration prior keeps its correlations; the mounting factors are checked on an asymmetric mounting. |
| `vehicle_estimator_test_calibration_policy` | unit | The prior hash against golden values computed independently, what does and does not change it, every write-policy branch, load inflation, floor and cap, and malformed estimates refused. |
| `vehicle_estimator_test_mounting` | slow | A 2° mounting yaw learned on straights to 0.02°, roll and pitch learned at stops, a drift teaching the mounting nothing, and a knocked IMU followed with the walk and not without it. |
| `vehicle_estimator_test_offline` | unit | The batch is never worse than the fixed-lag smoother and is clearly better at the start and across an outage; its calibration history carries what was learned late back to the start of the drive. |
| `vehicle_estimator_test_solver` | unit | What a keyframe costs: two iterations, no rejected steps, the covariance from the solve's own factorisation, and the same accuracy. None of these shows in the estimate, only in the time. |
| `vehicle_estimator_test_keyframes` | unit | Inertial keyframes through an outage and none beside GNSS; a late epoch still used; the buffer bounded; sigmas growing; parked held at zero velocity. |
| `vehicle_estimator_test_barometer` | slow | Offset to a metre and airflow to 0.1 from hills; a 60 s outage and an autonomous fix held by it; the weather followed. |
| `vehicle_estimator_test_magnetometer` | slow | Hard and soft iron learned, small and large; five minutes parked without a heading held by it; an outage; a disturbance refused. |
| `vehicle_estimator_test_cold_start` | slow | No GNSS at power-on: attitude valid with real sigmas, magnetic heading to 3° from a learned calibration, none from an unlearned one; one re-anchor to true heading, at 49° N and 60° S. |
| `vehicle_estimator_test_long_outage` | slow | Five minutes without GNSS: a covariance on every keyframe, attitude valid throughout, position sigma past 12 m and still covering the error, and a clean recovery. |
| `vehicle_estimator_test_gravity` | slow | Laps in the Colorado foothills with deflected truth gravity: modelled, the attitude's mean tilt error matches a world with no deflection; ignored, it leans by the deflection. |
| `vehicle_estimator_test_consistency` | slow | NEES over twenty noisy figure-of-eight drives stays within bounds, so the reported sigmas mean what they say. |

On the skidpad, figure of eight and spin, the fixed-lag smoother holds
position to about 3.5 cm, and attitude and sideslip to under 0.3°. On a
skidpad with a 5 s outage, the offline batch cut RMS yaw error from 0.044° to
0.008° and RMS sideslip error from 0.045° to 0.011°. Through the node's capnp
path the worst yaw error was 0.09° and the worst sideslip error 0.12°. The
sideslip sign and the vertical-velocity sign were both mutation-checked.
