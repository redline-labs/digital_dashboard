"""Test cases for the SymForce oracle. Each has a C++ twin in libs/csym/tests/oracle_cases.hpp: keep the math in sync.

Argument order, types, and `wrt` must match the C++ declarations exactly.
"""

from __future__ import annotations

import dataclasses
import typing as T

import numpy as np

import sym
import symforce.symbolic as sf
from symforce.opt import noise_models as nm


@dataclasses.dataclass
class Case:
    name: str
    doc: str
    func: T.Callable
    wrt: list[str]
    samples: T.Callable[[np.random.Generator], list[tuple]]


EPS = 1e-10


def n_samples(make: T.Callable[[np.random.Generator], tuple], extra: T.Sequence[tuple] = (), n: int = 12):
    def gen(rng: np.random.Generator) -> list[tuple]:
        return [make(rng) for _ in range(n)] + list(extra)

    return gen


def rot3(rng: np.random.Generator, scale: float = 1.0) -> sym.Rot3:
    return sym.Rot3.from_tangent(rng.normal(size=3) * scale, epsilon=EPS)


def rot2(rng: np.random.Generator) -> sym.Rot2:
    return sym.Rot2.from_tangent(np.array([rng.uniform(-3, 3)]), epsilon=EPS)


def pose2(rng: np.random.Generator) -> sym.Pose2:
    return sym.Pose2(R=rot2(rng), t=rng.normal(size=2))


def pose3(rng: np.random.Generator) -> sym.Pose3:
    return sym.Pose3(R=rot3(rng), t=rng.normal(size=3))


def vec(rng: np.random.Generator, n: int, lo: float = -2, hi: float = 2) -> np.ndarray:
    return rng.uniform(lo, hi, size=n)


# ---- scalar ops -----------------------------------------------------------------------------------


def arith(x: sf.Scalar, y: sf.Scalar) -> sf.Scalar:
    return x * y + x / y - 3 * x + y**2 - (x - y) ** 3 / (1 + x**2)


def trig(x: sf.Scalar, y: sf.Scalar) -> sf.V3:
    return sf.V3(sf.sin(x) * sf.cos(y), sf.tan(x * y / 4), sf.sin(x + y) ** 2 - sf.cos(2 * x))


def inverse_trig(x: sf.Scalar, y: sf.Scalar) -> sf.V3:
    return sf.V3(sf.asin(x / 3), sf.acos(y / 3), sf.atan(x * y))


def atan2_eps(y: sf.Scalar, x: sf.Scalar, epsilon: sf.Scalar) -> sf.Scalar:
    return sf.atan2(y, x, epsilon=epsilon)


def exp_log(x: sf.Scalar, y: sf.Scalar) -> sf.V3:
    return sf.V3(sf.exp(x) * sf.log(y), sf.tanh(x - y), sf.log(1 + sf.exp(-x * x)))


def powers(x: sf.Scalar, y: sf.Scalar) -> sf.V4:
    return sf.V4(x**y, x**3 - 2 * x**-2, sf.sqrt(y) + y ** sf.S(-1.5), (x * y) ** sf.S(0.5))


def piecewise(x: sf.Scalar, y: sf.Scalar) -> sf.V4:
    return sf.V4(sf.Min(x, y) * sf.Max(x, 2 * y), sf.Abs(x - y) * x, sf.sign(x) * y**2, sf.Max(x * y, 0))


def nested(x: sf.Scalar, y: sf.Scalar, epsilon: sf.Scalar) -> sf.Scalar:
    return sf.sin(sf.exp(x / 2) + sf.sqrt(x**2 + y**2 + epsilon**2)) / (2 + sf.cos(x * y))


def norm_eps(v: sf.V3, epsilon: sf.Scalar) -> sf.V3:
    return v / v.norm(epsilon=epsilon)


# ---- geometry -------------------------------------------------------------------------------------


def rot3_act(R: sf.Rot3, p: sf.V3) -> sf.V3:
    return R * p


def rot3_compose(a: sf.Rot3, b: sf.Rot3) -> sf.M33:
    return (a * b).to_rotation_matrix()


def rot3_local(a: sf.Rot3, b: sf.Rot3, epsilon: sf.Scalar) -> sf.V3:
    return sf.V3(a.local_coordinates(b, epsilon=epsilon))


def rot3_from_tangent(v: sf.V3, epsilon: sf.Scalar) -> sf.M33:
    return sf.Rot3.from_tangent(v, epsilon=epsilon).to_rotation_matrix()


def rot2_local(a: sf.Rot2, b: sf.Rot2, epsilon: sf.Scalar) -> sf.V1:
    return sf.V1(a.local_coordinates(b, epsilon=epsilon))


def pose2_between(a: sf.Pose2, b: sf.Pose2, a_T_b: sf.Pose2, sqrt_info: sf.M33, epsilon: sf.Scalar) -> sf.V3:
    return sqrt_info * sf.V3(a_T_b.local_coordinates(a.between(b), epsilon=epsilon))


def pose3_between(a: sf.Pose3, b: sf.Pose3, a_T_b: sf.Pose3, sqrt_info: sf.M66, epsilon: sf.Scalar) -> sf.V6:
    return sqrt_info * sf.V6(a_T_b.local_coordinates(a.between(b), epsilon=epsilon))


def pose3_prior(x: sf.Pose3, prior: sf.Pose3, sqrt_info: sf.M66, epsilon: sf.Scalar) -> sf.V6:
    return sqrt_info * sf.V6(prior.local_coordinates(x, epsilon=epsilon))


def pose3_inverse_act(T_: sf.Pose3, p: sf.V3) -> sf.V3:
    return T_.inverse() * p


# Lie-group outputs: Jacobian rows are in the output's tangent space.
def rot3_compose_group(a: sf.Rot3, b: sf.Rot3) -> sf.Rot3:
    return a * b


def pose3_compose_group(a: sf.Pose3, b: sf.Pose3) -> sf.Pose3:
    return a * b


def pose2_between_group(a: sf.Pose2, b: sf.Pose2) -> sf.Pose2:
    return a.between(b)


# ---- noise models ----


def barron_whiten(v: sf.V3, alpha: sf.Scalar, s: sf.Scalar, epsilon: sf.Scalar) -> sf.V3:
    return sf.V3(nm.BarronNoiseModel(alpha=alpha, scalar_information=s, x_epsilon=epsilon).whiten(v))


def barron_whiten_norm(v: sf.V3, alpha: sf.Scalar, s: sf.Scalar, delta: sf.Scalar, epsilon: sf.Scalar) -> sf.V3:
    model = nm.BarronNoiseModel(alpha=alpha, scalar_information=s, x_epsilon=epsilon, delta=delta)
    return sf.V3(model.whiten_norm(v, epsilon=epsilon))


def pseudo_huber_whiten_norm(v: sf.V2, delta: sf.Scalar, s: sf.Scalar, epsilon: sf.Scalar) -> sf.V2:
    model = nm.PseudoHuberNoiseModel(delta=delta, scalar_information=s, epsilon=epsilon)
    return sf.V2(model.whiten_norm(v, epsilon=epsilon))


# ---- vehicle --------------------------------------------------------------------------------------


def pacejka(alpha: sf.Scalar, B: sf.Scalar, C: sf.Scalar, D: sf.Scalar, E: sf.Scalar) -> sf.Scalar:
    """Simplified Magic Formula lateral force."""
    x = B * alpha
    return D * sf.sin(C * sf.atan(x - E * (x - sf.atan(x))))


def tire_fy(vx: sf.Scalar, vy: sf.Scalar, r: sf.Scalar, delta: sf.Scalar, a: sf.Scalar,
            B: sf.Scalar, C: sf.Scalar, mu: sf.Scalar, Fz: sf.Scalar, E: sf.Scalar,
            epsilon: sf.Scalar) -> sf.Scalar:
    """Front tire lateral force from the slip angle of a wheel at distance a ahead of the CG."""
    alpha = delta - sf.atan2(vy + a * r, vx, epsilon=epsilon)  # restoring-force convention
    return pacejka(alpha, B, C, mu * Fz, E)


def bicycle_factor(pose_k: sf.Pose2, vel_k: sf.V3, pose_k1: sf.Pose2, vel_k1: sf.V3, u: sf.V2,
                   chassis: sf.V4, tire_f: sf.V4, tire_r: sf.V4, dt: sf.Scalar, sqrt_info_diag: sf.V6,
                   epsilon: sf.Scalar) -> sf.V6:
    """Dynamic bicycle model, explicit Euler step, as a residual between consecutive states.

    vel = [vx, vy, r] (body frame), u = [delta, Fx], chassis = [m, Iz, lf, lr],
    tire_f / tire_r = Pacejka [B, C, D, E].
    """
    vx, vy, r = vel_k
    delta, Fx = u
    m, Iz, lf, lr = chassis
    Bf, Cf, Df, Ef = tire_f
    Br, Cr, Dr, Er = tire_r
    alpha_f = delta - sf.atan2(vy + lf * r, vx, epsilon=epsilon)
    alpha_r = -sf.atan2(vy - lr * r, vx, epsilon=epsilon)
    Fyf = pacejka(alpha_f, Bf, Cf, Df, Ef)
    Fyr = pacejka(alpha_r, Br, Cr, Dr, Er)
    vx_dot = (Fx - Fyf * sf.sin(delta)) / m + vy * r
    vy_dot = (Fyr + Fyf * sf.cos(delta)) / m - vx * r
    r_dot = (lf * Fyf * sf.cos(delta) - lr * Fyr) / Iz
    step = sf.Pose2(R=sf.Rot2.from_angle(r * dt), t=sf.V2(vx * dt, vy * dt))
    pose_pred = pose_k * step
    vel_pred = sf.V3(vx + vx_dot * dt, vy + vy_dot * dt, r + r_dot * dt)
    res_pose = sf.V3(pose_pred.local_coordinates(pose_k1, epsilon=epsilon))
    res_vel = vel_k1 - vel_pred
    res = sf.V6(res_pose[0], res_pose[1], res_pose[2], res_vel[0], res_vel[1], res_vel[2])
    return sf.V6([res[i] * sqrt_info_diag[i] for i in range(6)])


def robust_tire_factor(vx: sf.Scalar, vy: sf.Scalar, r: sf.Scalar, delta: sf.Scalar, a: sf.Scalar,
                       B: sf.Scalar, C: sf.Scalar, mu: sf.Scalar, Fz: sf.Scalar, E: sf.Scalar,
                       fy_measured: sf.Scalar, sigma: sf.Scalar, alpha: sf.Scalar, epsilon: sf.Scalar) -> sf.V1:
    """Measured lateral force vs the Pacejka model, whitened and robustified with an adaptive Barron loss."""
    fy = tire_fy(vx, vy, r, delta, a, B, C, mu, Fz, E, epsilon)
    model = nm.BarronNoiseModel(alpha=alpha, scalar_information=1, x_epsilon=epsilon)
    return sf.V1(model.whiten_norm(sf.V1((fy - fy_measured) / sigma), epsilon=epsilon))


def robust_pose2_between(a: sf.Pose2, b: sf.Pose2, a_T_b: sf.Pose2, sigmas: sf.V3, epsilon: sf.Scalar) -> sf.V3:
    """Pose2 between factor with a Cauchy loss on the whitened residual block."""
    r = sf.V3(a_T_b.local_coordinates(a.between(b), epsilon=epsilon))
    w = sf.V3([r[i] / sigmas[i] for i in range(3)])
    model = nm.BarronNoiseModel(alpha=0, scalar_information=1, x_epsilon=epsilon)
    return sf.V3(model.whiten_norm(w, epsilon=epsilon))


def bicycle_sample(rng: np.random.Generator) -> tuple:
    vx = rng.uniform(5, 40)
    chassis = np.array([rng.uniform(600, 900), rng.uniform(800, 1500), rng.uniform(1.0, 1.6), rng.uniform(1.2, 1.8)])
    tire = lambda: np.array([rng.uniform(8, 14), rng.uniform(1.2, 1.6), rng.uniform(5000, 9000),  # noqa: E731
                             rng.uniform(-0.5, 0.5)])
    return (pose2(rng), np.array([vx, rng.uniform(-2, 2), rng.uniform(-1, 1)]), pose2(rng),
            np.array([vx + rng.uniform(-1, 1), rng.uniform(-2, 2), rng.uniform(-1, 1)]),
            np.array([rng.uniform(-0.2, 0.2), rng.uniform(-3000, 3000)]), chassis, tire(), tire(),
            rng.uniform(0.005, 0.05), rng.uniform(0.5, 2.0, size=6), EPS)


# ---- registry -------------------------------------------------------------------------------------

M66 = lambda rng: np.eye(6) + 0.1 * rng.normal(size=(6, 6))  # noqa: E731
M33 = lambda rng: np.eye(3) + 0.1 * rng.normal(size=(3, 3))  # noqa: E731

CASES = [
    Case("arith", "rational arithmetic", arith, ["x", "y"],
         n_samples(lambda r: (r.uniform(-2, 2), r.uniform(0.5, 2)))),
    Case("trig", "sin/cos/tan", trig, ["x", "y"], n_samples(lambda r: (r.uniform(-3, 3), r.uniform(-3, 3)))),
    Case("inverse_trig", "asin/acos/atan", inverse_trig, ["x", "y"],
         n_samples(lambda r: (r.uniform(-2, 2), r.uniform(-2, 2)))),
    Case("atan2_eps", "atan2 with epsilon, including x = 0", atan2_eps, ["y", "x"],
         n_samples(lambda r: (r.uniform(-2, 2), r.uniform(-2, 2), EPS), extra=[(1.0, 0.0, EPS), (-0.5, 0.0, EPS)])),
    Case("exp_log", "exp/log/tanh", exp_log, ["x", "y"], n_samples(lambda r: (r.uniform(-2, 2), r.uniform(0.1, 3)))),
    Case("powers", "integer, negative, fractional, symbolic powers", powers, ["x", "y"],
         n_samples(lambda r: (r.uniform(0.2, 2), r.uniform(0.2, 2)))),
    Case("piecewise", "min/max/abs/sign", piecewise, ["x", "y"],
         n_samples(lambda r: (r.uniform(-2, 2), r.uniform(-2, 2)))),
    Case("nested", "compositions with epsilon under sqrt", nested, ["x", "y"],
         n_samples(lambda r: (r.uniform(-2, 2), r.uniform(-2, 2), EPS), extra=[(0.0, 0.0, EPS)])),
    Case("norm_eps", "vector normalization with epsilon", norm_eps, ["v"],
         n_samples(lambda r: (vec(r, 3), EPS))),
    Case("rot3_act", "Rot3 * point", rot3_act, ["R", "p"], n_samples(lambda r: (rot3(r), vec(r, 3)))),
    Case("rot3_compose", "rotation matrix of a composition", rot3_compose, ["a", "b"],
         n_samples(lambda r: (rot3(r), rot3(r)))),
    Case("rot3_local", "Rot3 local coordinates", rot3_local, ["a", "b"],
         n_samples(lambda r: (rot3(r), rot3(r), EPS))),
    Case("rot3_from_tangent", "Rot3 exponential map", rot3_from_tangent, ["v"],
         n_samples(lambda r: (vec(r, 3), EPS), extra=[(np.array([1e-4, -2e-4, 3e-5]), EPS)])),
    Case("rot2_local", "Rot2 local coordinates", rot2_local, ["a", "b"],
         n_samples(lambda r: (rot2(r), rot2(r), EPS))),
    Case("pose2_between", "Pose2 between factor", pose2_between, ["a", "b"],
         n_samples(lambda r: (pose2(r), pose2(r), pose2(r), M33(r), EPS))),
    Case("pose3_between", "Pose3 between factor", pose3_between, ["a", "b"],
         n_samples(lambda r: (pose3(r), pose3(r), pose3(r), M66(r), EPS), n=8)),
    Case("pose3_prior", "Pose3 prior factor", pose3_prior, ["x"],
         n_samples(lambda r: (pose3(r), pose3(r), M66(r), EPS), n=8)),
    Case("pose3_inverse_act", "Pose3 inverse * point", pose3_inverse_act, ["T_", "p"],
         n_samples(lambda r: (pose3(r), vec(r, 3)))),
    Case("rot3_compose_group", "Rot3 composition, Rot3 output", rot3_compose_group, ["a", "b"],
         n_samples(lambda r: (rot3(r), rot3(r)))),
    Case("pose3_compose_group", "Pose3 composition, Pose3 output", pose3_compose_group, ["a", "b"],
         n_samples(lambda r: (pose3(r), pose3(r)))),
    Case("pose2_between_group", "Pose2 between, Pose2 output", pose2_between_group, ["a", "b"],
         n_samples(lambda r: (pose2(r), pose2(r)))),
    Case("barron_whiten", "Barron loss, per-element whitening", barron_whiten, ["v", "alpha"],
         n_samples(lambda r: (vec(r, 3, -3, 3), r.uniform(-3, 1.8), r.uniform(0.5, 2), 1e-6))),
    Case("barron_whiten_norm", "Barron loss on the residual norm", barron_whiten_norm, ["v", "alpha", "delta"],
         n_samples(lambda r: (vec(r, 3, -3, 3), r.uniform(-3, 1.8), r.uniform(0.5, 2), r.uniform(0.5, 2), 1e-6))),
    Case("pseudo_huber_whiten_norm", "pseudo-Huber on the residual norm", pseudo_huber_whiten_norm, ["v", "delta"],
         n_samples(lambda r: (vec(r, 2, -3, 3), r.uniform(0.5, 2), r.uniform(0.5, 2), 1e-6))),
    Case("tire_fy", "Pacejka lateral force from vehicle states", tire_fy,
         ["vx", "vy", "r", "delta", "B", "C", "mu", "E"],
         n_samples(lambda r: (r.uniform(2, 40), r.uniform(-3, 3), r.uniform(-1, 1), r.uniform(-0.3, 0.3),
                              r.uniform(1.0, 1.6), r.uniform(8, 14), r.uniform(1.2, 1.6), r.uniform(0.9, 1.3),
                              r.uniform(3000, 6000), r.uniform(-0.5, 0.5), EPS))),
    Case("robust_tire_factor", "Pacejka force measurement with adaptive Barron loss", robust_tire_factor,
         ["vx", "vy", "r", "delta", "B", "C", "mu", "E", "alpha"],
         n_samples(lambda r: (r.uniform(5, 40), r.uniform(-2, 2), r.uniform(-1, 1), r.uniform(-0.2, 0.2), 1.3,
                              r.uniform(8, 14), r.uniform(1.2, 1.6), r.uniform(0.9, 1.3), r.uniform(3000, 6000),
                              r.uniform(-0.5, 0.5), r.uniform(-3000, 3000), r.uniform(50, 300), r.uniform(-2, 1.5),
                              1e-6))),
    Case("robust_pose2_between", "Pose2 between factor with a Cauchy loss", robust_pose2_between, ["a", "b"],
         n_samples(lambda r: (pose2(r), pose2(r), pose2(r), r.uniform(0.05, 0.5, size=3), 1e-6))),
    Case("bicycle_factor", "dynamic bicycle model Euler residual", bicycle_factor,
         ["pose_k", "vel_k", "pose_k1", "vel_k1"], n_samples(bicycle_sample, n=8)),
]
