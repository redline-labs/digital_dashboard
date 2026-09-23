---
title: geodesy
parent: Libraries
---

# geodesy

## Overview

The WGS 84 ellipsoid. The library covers:

- geodetic to ECEF and back
- the local NED frame
- the two radii of curvature
- normal gravity
- the earth's rotation rate

It is header-only. Every function is a template over the scalar and is
branch-free, so the same code runs in three places: on `double` at runtime,
inside a `static_assert`, and on `csym::Expr` inside a factor that
[csym](csym.html) differentiates. For the same reason its maths goes through
csym's front-ends, which are `constexpr` during constant evaluation and call
`<cmath>` otherwise.

The inverse, `ecefToLlh`, uses Vermeille's closed form rather than the usual
iteration. A loop that stops on a tolerance cannot be traced.

This is the tree's one ellipsoid. [wmm](wmm.html) takes its constants from
here rather than carrying a copy. The consumers are [imu_preint](imu_preint.html)
and [vehicle_estimator](vehicle_estimator.html).

## Public headers

| Header | |
| --- | --- |
| `geodesy/wgs84.h` | The constants, from NIMA TR8350.2. The four defining parameters (`kA`, `kInvF`, `kGM`, `kOmegaIe`) are authoritative. `kB`, `kE2` and `kEp2` are derived from them. The Somigliana constants are copied from the published table. |
| `geodesy/geodetic.h` | `Llh<T>`, `llhToEcef`, `ecefToLlh`, `rotEcefFromNed`, `primeVerticalRadius`, `meridianRadius`, and the checked entry points `isPlausible` and `isPlausibleEcef`. |
| `geodesy/gravity.h` | `normalGravitySurface`, `normalGravityNed`, `normalGravityEcef`, `kOmegaIeEcef`, and the runtime seam `GravityModel` with its first implementation `NormalGravity`. |

## Using it

Link `geodesy`. It brings `csym` with it. Angles are radians throughout, and
heights are above the ellipsoid.

```cpp
const geodesy::Llh<double> llh{lat_rad, lon_rad, h_m};
if (!geodesy::isPlausible(llh)) return;               // a measurement, so check it
const csym::Vector3<double> p_e = geodesy::llhToEcef(llh);
const csym::Matrix33<double> R_e_n = geodesy::rotEcefFromNed(llh.lat, llh.lon);
const csym::Vector3<double> v_e = R_e_n * v_ned;      // columns are N, E, D in ECEF
```

The estimator holds a `GravityModel` and never knows which one it has. Gravity
is evaluated once at a position estimate and handed to the IMU factor as a
constant. Over one IMU interval, gravity changes with position error by about
3e-6 s⁻² per metre, which is far below anything the accelerometer resolves, so
gravity does not need a Jacobian.

## Behaviour worth knowing

The failure mode this library guards against is a plausible-looking wrong
latitude. `isPlausible` refuses:

- NaN and infinity
- a latitude past either pole
- a height more than 100 km from the ellipsoid (`kMaxAbsHeightM`)

The height limit also catches degrees passed where radians were expected,
because 47 "radians" of latitude fails the pole check. `isPlausibleEcef`
applies the same limits to a Cartesian point. Code holding a receiver's
report should call these before converting.

`ecefToLlh` is exact everywhere outside the ellipsoid's evolute, a region
within about 43 km of the earth's centre. The height limit keeps every input
well clear of it.

"Normal gravity" means the ellipsoid's own field:

- Somigliana's closed form at the surface
- TR8350.2's second-order expansion with height
- the small north component that appears off the surface, `-8.08e-9 · h ·
  sin 2φ` (Groves eq. 2.140)

It is a few micro-g at track altitudes, and it is included because leaving it
out would be an approximation the rest of the model does not make.

Normal gravity is not the real field. The real field has anomalies, and its
plumb line leans up to tens of arcseconds off the ellipsoid normal. EGM2008 or
DEFLEC would add those, and would plug in as another `GravityModel`. As of
2026-09-23 neither is implemented.

`kOmegaIeEcef` is the earth's rotation, `[0, 0, ω]` in ECEF.

## Tests

Both carry the labels `geodesy unit`.

| Target | What it proves |
| --- | --- |
| `geodesy_test_geodetic` | The round trip over a grid that includes the poles and the antimeridian. It also checks `ecefToLlh` against an independent solver written in the test: Bowring's iteration, which shares no intermediate with Vermeille. They agree to within a micrometre. Points whose answer is pure geometry (the equator, the poles, a metre straight up) are pinned in `static_assert`s. The test also covers input rejection, and the Jacobian traced through csym against finite differences. |
| `geodesy_test_gravity` | The published equator and pole values (9.7803253359 and 9.8321849378 m/s²), and the copied constants re-derived from the defining four. It also checks the whole vector against the gradient of the normal potential, written as GM with its even zonal harmonics J2 to J8 plus the centrifugal term. That is different physics, and it must give the same vector. |

A round trip alone proves little: `ecefToLlh` could perfectly invert a wrong
`llhToEcef`. That is why both tests lean on oracles that share nothing with the
code under test.
