---
title: factor_graph
parent: Libraries
---

# factor_graph

## Overview

A nonlinear least-squares factor graph, plus the two smoothers the state
estimator runs on it: a fixed-lag smoother for the car and a batch smoother
for a whole drive after the fact. Residuals come from [csym](csym.html) and
are differentiated at compile time. The linear algebra is Eigen's.

The library knows nothing about sensors, frames or time bases. A factor is a
whitened residual over a few keys and a stamp is a number, so the IMU,
GNSS and prior factors all live in [imu_preint](imu_preint.html) and
[vehicle_estimator](vehicle_estimator.html). GTSAM served as the reference
design. We wrote our own for three reasons: csym's compile-time Jacobians,
a dependency footprint that builds under Yocto, and a marginalisation rule we
can read in one file.

## Public headers

| Header | |
| --- | --- |
| `factor_graph/key.h` | `Key`, `symbol(kind, index)`, `keyName()`: a character and an index packed like `gtsam::Symbol`, so a log reads `x42`. |
| `factor_graph/values.h` | `Variable`, `VariableModel<T>` and `Values`. Any csym Lie group is a variable. `kEpsilon` is the one identity epsilon the whole graph shares. |
| `factor_graph/factor.h` | `Factor`, `Linearization`, and `factorProblem()`: the check every factor passes before it reaches the optimiser. |
| `factor_graph/csym_factor.h` | `CsymFactor<F, Vars<...>, Params<...>>`: a factor whose residual is a generic lambda. |
| `factor_graph/linear_prior.h` | `LinearPrior::fromHessian()`: what marginalisation leaves behind, frozen at its linearisation point. |
| `factor_graph/optimizer.h` | `LmParams`, `optimize()`, `totalCost()`, `jointCovariance()`, `jointCovariances()`. |
| `factor_graph/smoother.h` | `FixedLagSmoother`, `BatchSmoother`, `UpdateReport`, `updateProblem()`, `kStatic`. |

## Using it

Link the CMake target `factor_graph`. It brings in `csym` and `Eigen3::Eigen`
publicly. A factor is written once as a lambda over csym types:

{% raw %}
```cpp
constexpr auto kPrior = [](auto x, auto mean, auto sqrt_info) {
    return sqrt_info * (x - mean);
};
using PriorFactor = factor_graph::CsymFactor<kPrior, Vars<csym::Vector3<double>>,
                                             Params<csym::Vector3<double>, csym::Matrix33<double>>>;

factor_graph::FixedLagSmoother fls({.lag = 3.0});
factor_graph::Values v;
v.insert(symbol('x', 0), x0);
auto report = fls.update({std::make_shared<PriorFactor>("prior", {symbol('x', 0)}, mean, S)},
                         v, {{symbol('x', 0), t0}});
```
{% endraw %}

The first template arguments are the variables, looked up by key. The rest
are constants fixed when the factor is built. Each distinct instantiation
compiles a residual program, and a heavy one takes seconds. That is why the
estimator gives each factor type its own translation unit.

`BatchSmoother::add()` takes the same factors and values, with no stamps, and
`optimize()` solves them all at once.

## Behaviour worth knowing

**Marginal priors are frozen.** When a keyframe leaves the window, the
factors touching it are linearised, the Schur complement folds them onto
their Markov blanket, and `LinearPrior::fromHessian()` eigen-decomposes the
result into `0.5 |R d + e|^2`. Here `d` is the local coordinates from a
linearisation point that never moves again. Re-linearising at a later
estimate would invent information the discarded factors never carried. The
smoother would then become confidently wrong in the directions it cannot
observe, such as yaw before the car has turned. Eigen-directions below
`rank_tolerance` are dropped, so a prior's `dim()` is its rank.

**`kStatic` variables are never marginalised.** A lever arm or a mounting
angle stamped `kStatic` stays in the window for the smoother's whole life,
and the information about it builds up in the marginal priors. With lag 0 the
window holds exactly the newest state plus the statics.

**An update is all or nothing.** `updateProblem()` and `factorProblem()`
refuse a batch if any of these is true: a key is missing or duplicated, a
Jacobian has the wrong shape, or a residual is not finite. The smoother's
state is left exactly as it was, so one bad sensor sample cannot turn the
window to NaN.

**Convergence is a step size, not a cost change.** LM stops when an accepted
step is below `step_tolerance` in the whitened metric `sqrt(dᵀHd)`. A
relative-cost test stops early: the cost is dominated by the residual that
cannot be removed, and a parameter error `e` moves it only by `e²`. For the
same reason, a step whose predicted and actual decrease are both within the
cost's own rounding (64 ulp of the cost) is accepted rather than rejected.
Without that rule, LM near the answer rejected steps until lambda saturated.

**The LDLT is refactorised from scratch on every iteration.** The sparsity
pattern of the normal equations can change between iterations: a coupling
Jacobian that is exactly zero at the start point is non-zero one step later.
Reusing an `analyzePattern()` from the first iteration wrote past the end of
Eigen's arrays. The result was heap corruption that surfaced as a crash
minutes into a drive, and it was found under ASan.
`factor_graph_test_graph` has the case, and the mutant crashes.

**Unobservable means empty, not large.** `jointCovariance()` returns
`std::nullopt` when the Hessian is singular. The inverse of a near-singular
matrix is a plausible wrong answer. `jointCovariances()` answers many groups
of keys from one factorisation, which is what keeps the offline smoother
linear in drive length.

## Tests

`factor_graph_test_kalman` (unit) is the oracle. On linear-Gaussian chains
in 1-D and 6-D, the fixed-lag smoother at lag 0 must reproduce a textbook
Kalman filter's mean and `P(k|k)` at every step. At lag L it must match the
RTS smoother over the data so far. `BatchSmoother` must match RTS at every
state and covariance. All three hold to 1e-9, with deliberately bad initial
guesses. This is the proof that the batch path is RTS, and that the offline
smoother is its nonlinear generalisation.

`factor_graph_test_graph` (unit) covers the nonlinear side:

- a rotation chain started 170° from the answer
- the marginal prior's `RᵀR = H` and its Jacobian against finite differences
- Rosenbrock under LM
- the changing-sparsity case above
- a gauge-free problem that converges without NaN and reports no covariance
- a static bias that lag 0 must estimate exactly as the batch does
- every malformed update being refused, with the state left untouched
