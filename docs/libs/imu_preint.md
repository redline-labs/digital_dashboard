---
title: imu_preint
parent: Libraries
---

# imu_preint

## Overview

IMU preintegration from strapdown increments, and the two factors the IMU
puts in a [factor_graph](factor_graph.html): the preintegrated interval
between two keyframes, and the random walk that ties consecutive biases.
Alongside those, it has a sample sequencer for an MTi's wrapping counters and
a truth simulator that turns a trajectory into the increments an ideal IMU
would report.

The input is the sensor's own coning- and sculling-compensated `dq` and `dv`
(the MTi's SDI outputs), not rates. The sensor integrated them at 2 kHz
internally, so accumulating its results keeps a fast yaw transient from
becoming a heading error. Beyond "gives `dq` and `dv`" the library is
sensor-agnostic. The algebra is Forster et al., *On-Manifold Preintegration*
(TRO 2017), adapted to the rotating ECEF frame. Which variables make a
keyframe, and when one is made, belongs to
[vehicle_estimator](vehicle_estimator.html).

## Public headers

| Header | |
| --- | --- |
| `imu_preint/increment.h` | `Increment`, `DvFrame`, `split()` (cut an increment at a keyframe), `incrementProblem()`, rotation-vector helpers. |
| `imu_preint/preintegrator.h` | `NoiseParams` (MTi-610 datasheet defaults), `Preintegrated`, `Preintegrator`, `rightJacobian()`, `skew()`. |
| `imu_preint/imu_model.h` | `predict<T>()`: where an interval puts the second keyframe, in ECEF. Generic over the scalar, so the factor and the runtime propagation share it. |
| `imu_preint/imu_factor.h` | `KeyframeKeys`, `makeImuFactor()`, `makeBiasWalkFactor()`, `NavState`, `predict()`. |
| `imu_preint/sequencer.h` | `SampleSequencer`: packet counter and SampleTimeFine unwrapped, with gaps classified. |
| `imu_preint/sim.h` | `Trajectory`, `LocalTrajectory`, `simulateIncrements()`, `yawPitchRoll()`. |

## Using it

Link the CMake target `imu_preint`, which brings in `factor_graph` and
`geodesy`:

```cpp
imu_preint::Preintegrator pim(noise, imu_preint::DvFrame::end, bg, ba);
for (const auto& inc : since_last_keyframe)
    if (auto err = pim.integrate(inc); !err.empty()) { /* refused, state untouched */ }

auto f = imu_preint::makeImuFactor(keys_i, keys_j, pim.result(),
                                   gravity_e_at_i, omega_ie);  // geodesy::kOmegaIeEcef as Eigen
```

A keyframe that falls inside a sample takes its share through `split()`,
which assumes constant rate and force within the sample. A dropped sample is
bridged by integrating a stand-in with `extra_rot_var`/`extra_vel_var` and
`bridged = true`, so the covariance says how little the stand-in is trusted.

## Behaviour worth knowing

**The ECEF prediction includes earth rate beyond Coriolis.** `imu_model.h`
rotates the preintegrated force out of the frame it was summed in, as well as
applying `2ω×v`. The velocity gains `−ω×(RΔv·dt + RΔp) − ω×g·dt²` and the
position gains `−ω×v·dt² − ω×(RΔp)·dt − ⅓ω×g·dt³`. These terms are exact to
first order in `ωΔt` (7e-6 over a 0.1 s keyframe). A parked car found them:
without them, a stationary test drifted by `⅙ω×g·dt³`, which is 8e-5 m over
one second. The rotation `Exp(−ω dt) R_i ΔR` is exact for constant ω.

**Position gets a sub-sample slope correction.** The specific force within
one sample is taken to change linearly from the previous sample's. The
position integral subtracts `dt/12·(a_k − a_(k−1))`, with the previous force
carried across `reset()` in the frame the next sample starts in. Its bias
Jacobians include the slope term and the previous sample's force Jacobians.
Leaving them out made the bias-corrected `Δp` disagree with re-integration
on a skidpad.

**`DvFrame` is a setting because it is unverified.** Xsens' manual writes
the SDI velocity update in the body frame at the end of the interval, and
`end` is the default. The two frames differ by one sample's rotation, which
is 0.01 rad at 1 rad/s and 100 Hz. Over a keyframe that becomes a systematic
velocity error. As of 2026-09-23 no MTi has confirmed which frame it uses.

**Counters wrap; gaps are events.** The packet counter is a UInt16, so it
wraps every 11 minutes at 100 Hz. SampleTimeFine counts at 10 kHz in 32 bits.
`SampleSequencer::push()` classifies each sample as one of `first`, `next`,
`gap` (with `missing`), `duplicate`, `out_of_order` or `restart`. A gap
longer than `max_bridge` is a restart, because the rotation during it cannot
be guessed.

**Increments are refused, not repaired.** `incrementProblem()` rejects:

- non-finite values
- a non-positive or implausibly long `dt`
- a quaternion more than 1e-3 from unit length

Within that tolerance the quaternion is renormalised.

**The simulator is built a different way on purpose.** `simulateIncrements()`
works from inertial-frame kinematics with Gauss-Legendre quadrature, not
from the ECEF equations in `imu_model.h`. A sign error in the model therefore
cannot be reproduced in the truth and cancel out.

## Tests

| Target | Label | What it proves |
| --- | --- | --- |
| `imu_preint_test_preintegrator` | unit | Bias Jacobians, slope term included, against re-integration; the SO(3) right Jacobian; `split()`; every refused increment. |
| `imu_preint_test_imu` | unit | The ECEF model against inertial-frame truth over a drift, a tumble and a parked car; what the earth rate and the dv frame are each worth; factor Jacobians against finite differences; a solve that recovers both biases. |
| `imu_preint_test_sequencer` | unit | Counter and tick wraps, gaps, duplicates, out-of-order samples, and a gap too long to bridge. |
| `imu_preint_test_covariance` | slow | The propagated covariance against 4000 noisy re-integrations. |

Removing the frame-rotation term or flipping the Coriolis sign fails
`imu_preint_test_imu`. Both mutations were checked.
