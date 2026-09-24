---
title: deflec
parent: Libraries
---

# deflec

## Overview

The deflection of the vertical is how far the plumb line (real gravity) leans
off the ellipsoid normal (normal gravity). The estimator uses it to point
gravity the right way. `deflec` reads NGS's gridded model files exactly as
NGS distributes them, memory-maps them, and interpolates them with NGS's own
bicubic. `DeflectedGravity` turns the result into a `geodesy::GravityModel`.

The library is given a directory and knows nothing about where it came from.
The files live verbatim in Git LFS under `models/deflec2022/` at the
repository root, which also installs them. The
[state_estimator](../nodes/state_estimator.html) node resolves them (see
[Where the model comes from](#where-the-model-comes-from)).

The model is `SDEFLEC2022 beta_v0a`, from NGS's NAPGD2022 beta: the North
America grids at 1 arcminute, 233 MB per component. It is a pre-release; a
new release replaces the two files and their names in `deflec/model.h`.

## Public headers

| Header | |
| --- | --- |
| `deflec/ngs_grid.h` | `NgsGrid`: one NGS `.bin`, opened and mapped, `at(lat_deg, lon_deg)`. `LoadError` and `LoadFailure` (the error, and a message naming the file and what to do). |
| `deflec/model.h` | `Model::open(dir)` (both components, which must share one grid), `at(lat_rad, lon_rad)` giving a `Deflection {xi_arcsec, eta_arcsec}`, and `DeflectedGravity`. |

## Using it

Link `deflec`.

```cpp
auto model = deflec::Model::open(dir);          // <dir>/SDEFLEC2022.NA.{eta,xi}.beta_v0a.bin
if (!model) { /* model.error().message says which file and why */ }
const deflec::DeflectedGravity gravity(std::make_shared<const deflec::Model>(std::move(*model)));
const auto g_e = gravity.gravityEcef(p_e);      // normal gravity outside the grid
const bool leaned = gravity.refinesNormalAt(p_e);
```

## Where the model comes from

- **In the repository:** `models/deflec2022/`, both files byte for byte as
  NGS publishes them, stored through Git LFS (`.gitattributes`). A checkout
  needs `git lfs install && git lfs pull`. `models/README.md` records the
  source and SHA-256 of each file.
- **Installed:** `models/CMakeLists.txt` installs the directory with
  `redline_install_data(... DESTINATION models/deflec2022 COMPONENT nodes)`,
  so an install has `<prefix>/models/deflec2022/` beside `<prefix>/bin/`, as
  the web console's assets are. The image recipe needs a `FILES` entry for
  `${REDLINE_PREFIX}/models` and must fetch LFS content.
- **At run time:** `core::paths::resource("models/deflec2022")` finds the
  installed directory beside `bin/`. In a developer build it walks up from
  the executable to the checkout. An install moved elsewhere still finds its
  own. `gravity.model_dir` in the node's config overrides this.

**An unfetched checkout cannot ship pointers.** Without `git lfs pull`, the
`.bin` files are ~130-byte text pointers. Three layers catch this:

- Configure warns, and installs nothing from `models/`.
- With `-DREDLINE_REQUIRE_MODELS=ON` (for the image build) configure fails.
- At run time, `NgsGrid::open` names a pointer as one (`LoadError::lfs_pointer`,
  "run `git lfs pull`"). The node then runs on normal gravity with a degraded
  `gravity` health check, rather than misreading 130 bytes as a grid.

## Behaviour worth knowing

**Signs are NGS's, and gravity leans against them.** xi is north-south and eta
east-west, in arcseconds, with xi = −∂N/∂north / R and eta = −∂N/∂east / R for
a geoid height N. Gravity's horizontal components are therefore −g·xi north
and −g·eta east. The sign was confirmed against the geoid rather than taken
from a description: across NGS's Guam grids, xi and eta match minus the
geoid's slopes with correlation 1.000 and slopes of 1.005 and 0.999.

**The interpolation is Catmull-Rom.** NGS specifies a local 4 × 4 bicubic,
with linear extrapolation padding the window within a node of an edge. Of the
kernels that name covers, only Catmull-Rom (Keys, a = −½) reproduces NGS's
published test values. It matches all 72 inside the North America grid to
their last digit (0.0005"). Bilinear misses by up to 3", biquadratic by 5",
and Keys a = −¾ by 1.5".

**The format is read in place.** An NGS `.bin` is little-endian: a 44-byte
header (float64 south, west, latitude spacing and longitude spacing, in
degrees with longitude east; int32 rows, columns and kind), then float32
values from south to north. A `static_assert` refuses a big-endian host. Every
header field is validated before any value is read: non-finite or
non-positive spacing, fewer than two rows or columns, an extent off the
globe. So is the file size, which must be exactly the header plus
rows × columns × 4. A header claiming two billion rows in a small file is a
size mismatch, never an out-of-bounds read.

**Memory.** The grids are `mmap`ed read-only. Only the pages under the car's
position are ever read, a few hundred kilobytes, not 467 MB.

**Outside the grid there is no answer.** The North America grid covers
0–90°N and 170–350°E. Elsewhere `Model::at` returns nothing and
`DeflectedGravity` gives normal gravity unchanged.

## Tests

| Target | Label | What it proves |
| --- | --- | --- |
| `deflec_test_grid` | unit | Synthetic grids written in NGS's format: every malformed file refused with its reason, including a Git LFS pointer, an absurd row count, a NaN edge, and two components on different grids. Interpolation is exact for linear fields up to the edges and for quadratics inside, and longitude works in any turn. Gravity leans −g·xi and −g·eta and keeps the down component; outside, and with no model, it is normal gravity. |
| `deflec_test_ngs` | unit | The real model, found as the node finds it, at all 72 NGS test points inside it, to 0.0006". It skips, saying why, when the checkout has no LFS content. |

The test coordinates are NGS's to eight decimals. Typed to four, they missed
Mount Whitney by 0.01", because on that slope 0.00005° is 0.01".
