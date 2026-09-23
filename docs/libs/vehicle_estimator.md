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
and both IMU biases). Two static variables live for the whole run: the
IMU-to-antenna lever arm and the antenna baseline's boresight. Between
keyframes the newest one is carried forward through the IMU samples, so the
output rate is the IMU's while the smoothing is the GNSS's.

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
| `vehicle_estimator/measurements.h` | `ImuSample`, `FixQuality`, `GnssPosition`, `GnssVelocity`, `DualAntenna`, `GnssEpoch` in; `VehicleState` out. |
| `vehicle_estimator/config.h` | `EstimatorConfig`: noise, geometry, gating, lag, start-up thresholds. Defaults describe an MTi-610 mounted x-forward, z-up and a BD992 on RTX. |
| `vehicle_estimator/clock.h` | `ClockOffset` and `TimeMapper`: both sensors on GPS time from host arrival times. |
| `vehicle_estimator/factors.h` | `gnssPosition`, `gnssVelocity`, `dualAntenna`, the priors, `Baseline`, `sqrtInformation`. |
| `vehicle_estimator/estimator.h` | `Estimator`, `EstimatorStatus`, `KeyframeRecord`. |
| `vehicle_estimator/offline.h` | `OfflineSmoother`: every keyframe of a drive solved together. |
| `vehicle_estimator/sim/scenario.h` | (target `vehicle_estimator_sim`) `skidpad`, `figureEight`, `parked`, `SensorModel`, `Scenario`. |

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
into sideslip, which is why `R_b_i` is configured and not estimated.

**Heading comes from the antennas, never from the track.** A drifting car's
course over ground is not its heading. The initialiser waits for a
dual-antenna yaw and builds the attitude by TRIAD from the baseline and the
specific force. It levels on the accelerometer after subtracting the
acceleration implied by successive GNSS velocities, so a start on the move is
level too. With no heading, no state is published.

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
restarts the smoother. The learned lever arm and boresight, with their
covariances, become the next start's priors. `KeyframeRecord::start` marks
those priors so the offline batch does not count them twice.

**Late GNSS gets a window.** An epoch becomes a keyframe only once the IMU is
`gnss_reorder_window` past it, so an epoch that arrives after its successor
still gets its turn. An epoch the IMU never covers is dropped after
`max_gnss_wait`.

**Unverified on hardware, as of 2026-09-23:**

- the MTi's `dv` frame (`DvFrame::end` is the default)
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
| `vehicle_estimator_test_factors` | unit | Each factor is zero at the truth, has finite-difference Jacobians, bends under the robust loss, wraps yaw, and includes the lever arm's rotational velocity. |
| `vehicle_estimator_test_offline` | unit | The batch is never worse than the fixed-lag smoother and is clearly better at the start and across an outage. |
| `vehicle_estimator_test_consistency` | slow | NEES over twenty noisy figure-of-eight drives stays within bounds, so the reported sigmas mean what they say. |

On the skidpad, figure of eight and spin, the fixed-lag smoother holds
position to about 3.5 cm, and attitude and sideslip to under 0.3°. On a
skidpad with a 5 s outage, the offline batch cut RMS yaw error from 0.044° to
0.008° and RMS sideslip error from 0.045° to 0.011°. Through the node's capnp
path the worst yaw error was 0.09° and the worst sideslip error 0.12°. The
sideslip sign and the vertical-velocity sign were both mutation-checked.
