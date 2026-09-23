---
title: csym
parent: Libraries
---

# csym

## Overview

Compile-time symbolic differentiation. A residual written once as a generic
lambda is traced into an expression graph during constant evaluation,
differentiated in each argument's tangent space, simplified, and lowered to a
fixed-size tape of straight-line code. What comes out is the value, the
Jacobian, `JᵀJ` and `Jᵀr` for a factor. That is SymForce's workflow with no
Python generator in the build and no generated source checked in. It is a port
of the standalone `sym_computation` project, reshaped to the tree's warning set
and naming.

csym does not solve anything. The optimizer that consumes its linearizations is
[factor_graph](factor_graph.html), and the estimator's residuals live in
[imu_preint](imu_preint.html) and [vehicle_estimator](vehicle_estimator.html).

csym keeps its own `Matrix` rather than using Eigen. Eigen's fixed-size
operations are not `constexpr`, because of SIMD intrinsics and alignment, so
the tracer cannot instantiate `Eigen::Matrix<csym::Expr>` inside constant
evaluation. csym's `Matrix` is column-major like Eigen's default. The boundary
is therefore a zero-copy `Eigen::Map`, and Eigen appears only where the
linear system is solved.

## Public headers

| Header | |
| --- | --- |
| `csym/csym.h` | Umbrella header. |
| `csym/function.h` | `csym::Function<F, Args...>`: `eval`, `jacobian`, `evaluate` (GTSAM-style per-argument blocks), `linearize`. All `constexpr`. |
| `csym/matrix.h` | Fixed-size column-major `Matrix`, `Vector3`, `Matrix33` over any scalar. |
| `csym/lie.h` | The Lie traits a value type opts into: `retract`, `local_coordinates`, `storage_D_tangent`. |
| `csym/geo/rot2.h`, `rot3.h`, `pose2.h`, `pose3.h` | The groups. `Rot3` is a quaternion with the perturbation on the right, as GTSAM does it. |
| `csym/geo/gtsam_conventions.h` | `GtsamPose2`/`GtsamPose3`: pose tangents ordered the way GTSAM orders them. |
| `csym/factors.h` | `prior_residual`, `between_residual` over any Lie group. |
| `csym/noise_models.h` | Isotropic, diagonal and sqrt-information whitening; `BarronNoiseModel` and `PseudoHuberNoiseModel` robust losses. |
| `csym/math/cmath.h` | `constexpr` elementary functions, with `<cmath>` at runtime. |
| `csym/core/batch.h` | `Batch<T, W>`: W factor instances evaluated at once in SIMD lanes. |
| `csym/emit.h` | Prints a compiled tape as readable C++. |
| `csym/adapters/eigen.h` | `to_eigen`, `from_eigen`, `eigen_map`. The only header that includes Eigen. |

`core/graph.h`, `core/simplify.h`, `lower.h`, `core/expr.h`, `core/storage.h`
and `core/buffer.h` are the machinery behind `Function`. A consumer does not
include them.

## Using it

Link `csym` (alias `csym::csym`). It is header-only.

```cpp
constexpr auto f = [](auto x, auto y) { return x * sin(y); };
using Fn = csym::Function<f, double, double>;

double v = Fn::eval(1.0, 0.3);
auto [value, J] = Fn::jacobian(1.0, 0.3);   // 1 x 2, tangent space
auto lin = Fn::linearize(1.0, 0.3);         // residual, jacobian, hessian, rhs
static_assert(Fn::eval(2.0, 0.0) == 0.0);   // works in constant evaluation too
```

A residual cannot branch on a traced value, because comparisons return an
`Expr` holding 0 or 1. Use `csym::where()`. Robust losses go inside the
residual: `whiten_norm` down-weights the whole residual block by its norm, and
the Jacobian includes the loss exactly rather than as an iteratively
reweighted approximation.

In this tree a residual becomes a factor through `factor_graph`'s `CsymFactor`.
Each factor gets its own translation unit, as in
`libs/vehicle_estimator/src/factors/`. Tracing a residual is the slow part of
the build. With one factor per TU, those traces run in parallel, and editing
one factor rebuilds only that factor.

## Behaviour worth knowing

The constexpr-limit flags on the `csym` interface target are required, not
tuning. Graph construction runs inside constant evaluation, and both
compilers' default step budgets stop it at a few hundred nodes, where a
preintegration residual needs thousands. They are interface options, so every
consumer gets them. A TU that somehow misses them fails with "constexpr
evaluation exceeded", and nothing in that message names csym's CMake file.
What constant evaluation costs, and why the library avoids `std::vector` in
its passes, is measured in the
[design note](../design/csym-constexpr.html).

`PseudoHuberNoiseModel::rho` computes `s x² / (sqrt(1 + s x²/δ²) + 1)` rather
than the textbook `δ² (sqrt(1 + s x²/δ²) - 1)`. The two are equal, but the
textbook form cancels to exactly zero once `s x²/δ²` drops below machine
epsilon. That happens with a large δ, or with a residual near zero, which is
exactly where the gradient still matters.

The standalone project shipped a GTSAM adapter. It was not brought over,
because this tree solves with `factor_graph`. `gtsam_conventions.h` stays: it
has no GTSAM dependency and only fixes tangent ordering.

The whole library builds as C++23 under the tree's strict warning set. As of
2026-09-23 it has been checked on clang and with a syntax-only strict pass on
GCC 15 (`g++-15` from Homebrew). The Yocto builder has not yet compiled it.

## Tests

All carry the labels `csym unit`.

| Target | What it proves |
| --- | --- |
| `csym_test_smoke` | The expression core, and evaluation inside `static_assert`. |
| `csym_test_geo_rot3`, `_geo_pose2`, `_geo_pose3` | Group Jacobians against finite differences. They are split into one TU each: together they took 151 s to compile. |
| `csym_test_oracle` | Values and tangent-space Jacobians against SymForce 0.12, using vectors checked in under `tests/vectors/`. |
| `csym_test_vehicle` | A Pacejka tire and a bicycle model: the kind of factor csym exists for. |
| `csym_test_batch` | The SIMD `Batch` kernels against scalar evaluation. |
| `csym_test_eigen` | The zero-copy map to Eigen, and Jacobians checked against analytic ones computed in Eigen. |

The oracle vectors come from `tools/csym_oracle`. It runs SymForce as a black
box and is not a build dependency, the same arrangement as the cantools pin in
[dbc_parser](dbc_parser.html). `symforce==0.12.0` is pinned exactly, because a
different release simplifies differently and would move every recorded op
count. Regenerate the vectors only when you add a case:

```bash
cd tools/csym_oracle && uv run oracle.py
```

The benchmarks under `bench/` build only with `-DCSYM_BENCH=ON`.
