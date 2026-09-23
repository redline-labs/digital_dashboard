---
title: estimator_offline
parent: Tools
---

# estimator_offline

## Overview

Two workstation tools for looking at the [state estimator](../nodes/state_estimator.html)
without a car. `estimator_sim` records a simulated drive: what `mti610_bridge`
and `bd992_bridge` would have published, on the same keys and schemas with
arrival jitter, plus the truth. `estimator_offline` runs the estimator over any
bag, simulated or real, twice over: once as the live node would have seen it,
and once as a whole-drive solve. Both write ordinary bags, so scope opens the
result.

```
 estimator_sim ─► sim bag ─► estimator_offline ─► estimate bag ─► scope (configs/scope/estimator.yaml)
                                 ▲
 bag record (a real drive) ──────┘
```

Neither ships. They link the simulator and hold a whole drive in memory, which
is why they are under `tools/`.

## Running it

```bash
./build/tools/estimator_sim/estimator_sim --out /tmp/skidpad --scenario skidpad --outage 25:30
./build/tools/estimator_offline/estimator_offline --in /tmp/skidpad --out /tmp/skidpad_est --time-offset 0.028
./build/apps/scope/scope --config configs/scope/estimator.yaml --bag /tmp/skidpad_est
```

`estimator_sim` prints the `--time-offset` its recording needs as its last
line: the simulated GNSS latency floor minus the IMU's. For a real drive, use
the value calibrated in `configs/state_estimator/state_estimator.yaml`.

### estimator_sim

| Option | Default | |
| --- | --- | --- |
| `-o, --out <dir>` | required | Bag directory to write. |
| `--scenario <name>` | `skidpad` | `skidpad`, `spin`, `figure8` or `parked`. |
| `--drive <s>` | `40` | Seconds of driving after the start. |
| `--seed <n>` | `1` | Sensor-noise seed. |
| `--outage <from:to>` | none | A GNSS outage, in seconds from the start. |

The bag holds `nodes/mti610/mtdata2/delta_q` and `delta_v` at 100 Hz, the
GSOF records the estimator reads under `nodes/bd992/gsof/` at 10 Hz (GSOF 12
and 38 at 1 Hz, as the shipped `bd992.yaml` sends them), and `sim/truth` as a
`VehicleState` at 100 Hz, so it plots against the estimate field by field.

Every scenario starts parked for 5 s, because the estimator levels itself on
a stationary accelerometer. `skidpad` then launches round a 40 m circle to
18 m/s and swings the slip angle between about 3° and 26° every 8 s; `spin`
is the same and ends by rotating the nose past 90° of slip; `figure8` drifts a
figure of eight with slip up to about 29° that changes sign at the crossing;
`parked` does not move.

### estimator_offline

| Option | Default | |
| --- | --- | --- |
| `-i, --in <dir>` | required | Input bag directory. |
| `-o, --out <dir>` | required | Output bag directory. |
| `-c, --config <file>` | `configs/state_estimator/state_estimator.yaml` | The node's config. |
| `--time-offset <s>` | from the config | Overrides `imu.time_offset_s`. |

The output is every input message, unchanged, plus two tracks:

| Key | Schema | What it is |
|---|---|---|
| `estimator/fls` | `VehicleState` | The fixed-lag estimate at IMU rate: what the car would have seen, through the same pipeline code the node runs. |
| `estimator/batch` | `VehicleState` | The whole drive solved at once from the fixed-lag run's keyframes, one state per keyframe (10 Hz). |

Exit codes: 0 when the batch solve converged, 1 when it did not, when the
estimator never started (no dual-antenna heading, or no IMU), or on a read or
write failure, and 2 for bad usage.

`estimator_offline` logs a summary: keyframes, states, resets and malformed
messages for the fixed-lag pass; keyframes, iterations, initial and final cost,
stop reason and refused factors for the batch.

## Reading the result

`configs/scope/estimator.yaml` lays out three panels (sideslip, speed and yaw),
each with `sim/truth`, `estimator/fls` and `estimator/batch`. On a real drive
there is no `sim/truth` and those traces stay empty.

The batch track is the nonlinear equivalent of an RTS smoother: every state
sees every measurement, before and after it. On a clean simulated drive it is
several times closer to the truth than the fixed-lag track (see the numbers in
the [design note](../design/state-estimation.html#measured-accuracy)).

{: .note }
Across a GNSS outage the batch track draws a straight line. A keyframe is made
per GNSS epoch, so the outage has none, and scope joins the two either side.
The fixed-lag track does not stop: between keyframes the IMU carries the state
forward at 100 Hz.

Batch states carry GPS time and the bag's clock is the host's. The fixed-lag
pass has both for every state it produced, so the tool takes the median of
host minus GPS time over those (one sample in fifty) and places each batch
state at its GPS time plus that offset. On a recording with a large clock step
partway through, the batch track will be shifted on one side of it.

## Checking the output

Use `scope_sample_stats` rather than a screenshot to compare the tracks: it
reports sample counts, drops and the minimum and maximum each trace actually
received. On the 45 s skidpad above, truth had 4499 samples with a minimum
sideslip of −25.78°, the fixed-lag track 4225 at −25.757°, and batch 373 at
−25.758°, with no drops.
