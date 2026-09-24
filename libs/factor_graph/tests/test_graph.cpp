// SPDX-License-Identifier: GPL-3.0-or-later
//
// The factor graph off the linear-Gaussian happy path: rotations, a curved
// valley, a direction nothing observes, a variable that is never
// marginalised, and every way an update can be malformed.

#include "factor_graph/csym_factor.h"
#include "factor_graph/linear_prior.h"
#include "factor_graph/smoother.h"

#include "csym/factors.h"
#include "csym/geo/rot3.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <type_traits>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

void near(double got, double want, double tolerance, const std::string& what)
{
    check(std::fabs(got - want) <= tolerance,
          fmt::format("{}: got {:.12g} want {:.12g} (tol {:.3g})", what, got, want, tolerance));
}

using factor_graph::FactorList;
using factor_graph::Key;
using factor_graph::symbol;
using factor_graph::Values;
using Rot3 = csym::Rot3<double>;
using V1 = csym::Vector<double, 1>;
using V3 = csym::Vector3<double>;
using M33 = csym::Matrix33<double>;

constexpr auto kRotPrior = [](auto r, auto mean, auto sqrt_info, auto eps) {
    return csym::prior_residual(r, mean, sqrt_info, eps);
};
using RotPriorFactor = factor_graph::CsymFactor<kRotPrior, factor_graph::Vars<Rot3>, factor_graph::Params<Rot3, M33, double>>;

constexpr auto kRotBetween = [](auto a, auto b, auto a_T_b, auto sqrt_info, auto eps) {
    return csym::between_residual(a, b, a_T_b, sqrt_info, eps);
};
using RotBetweenFactor =
    factor_graph::CsymFactor<kRotBetween, factor_graph::Vars<Rot3, Rot3>, factor_graph::Params<Rot3, M33, double>>;

constexpr auto kScalarPrior = [](auto x, auto mean, auto sigma) { return (x - mean) / sigma; };
using ScalarPriorFactor = factor_graph::CsymFactor<kScalarPrior, factor_graph::Vars<V1>, factor_graph::Params<V1, double>>;

constexpr auto kDifference = [](auto a, auto b, auto d, auto sigma) { return (b - a - d) / sigma; };
using DifferenceFactor =
    factor_graph::CsymFactor<kDifference, factor_graph::Vars<V1, V1>, factor_graph::Params<V1, double>>;

// z = x + bias
constexpr auto kBiased = [](auto x, auto bias, auto z, auto sigma) { return (x + bias - z) / sigma; };
using BiasedFactor = factor_graph::CsymFactor<kBiased, factor_graph::Vars<V1, V1>, factor_graph::Params<V1, double>>;

// Rosenbrock's valley as two residuals: 10 (y - x^2) and 1 - x.
constexpr auto kRosenbrock = [](auto x, auto y) {
    using T = std::remove_cvref_t<decltype(x[0])>;
    return csym::Vector<T, 2>{T(10) * (y[0] - x[0] * x[0]), T(1) - x[0]};
};
using RosenbrockFactor = factor_graph::CsymFactor<kRosenbrock, factor_graph::Vars<V1, V1>, factor_graph::Params<>>;

V1 v1(double x)
{
    V1 v;
    v[0] = x;
    return v;
}

M33 identity3()
{
    return M33::identity();
}

double angleBetween(const Rot3& a, const Rot3& b)
{
    return std::sqrt(csym::local_coordinates(a, b, factor_graph::kEpsilon).squared_norm());
}

// ---- rotations -------------------------------------------------------------

void testRotations()
{
    // A chain of three rotations anchored by a prior on the first, started
    // 170 degrees from the answer: far outside any small-angle regime.
    const Rot3 r0 = Rot3::from_yaw_pitch_roll(0.3, -0.2, 0.1);
    const Rot3 d01 = Rot3::from_yaw_pitch_roll(1.0, 0.2, -0.4);
    const Rot3 d12 = Rot3::from_yaw_pitch_roll(-0.6, 0.5, 0.9);
    const Key k0 = symbol('r', 0), k1 = symbol('r', 1), k2 = symbol('r', 2);

    FactorList f;
    f.push_back(std::make_shared<RotPriorFactor>("prior", std::array<Key, 1>{k0}, r0, identity3() * 10.0,
                                                 factor_graph::kEpsilon));
    f.push_back(std::make_shared<RotBetweenFactor>("between", std::array<Key, 2>{k0, k1}, d01, identity3() * 10.0,
                                                   factor_graph::kEpsilon));
    f.push_back(std::make_shared<RotBetweenFactor>("between", std::array<Key, 2>{k1, k2}, d12, identity3() * 10.0,
                                                   factor_graph::kEpsilon));

    Values v;
    const Rot3 far = Rot3::from_angle_axis(170.0 * std::numbers::pi / 180.0, V3{0.0, 0.6, 0.8});
    v.insert(k0, far);
    v.insert(k1, far * Rot3::from_yaw_pitch_roll(2.0, 0.0, 0.0));
    v.insert(k2, Rot3::identity());
    const auto report = factor_graph::optimize(f, v, {});
    check(report.converged, "rotation chain converged: " + report.stop_reason);
    near(angleBetween(v.at<Rot3>(k0), r0), 0.0, 1e-9, "r0");
    near(angleBetween(v.at<Rot3>(k1), r0 * d01), 0.0, 1e-9, "r1");
    near(angleBetween(v.at<Rot3>(k2), r0 * d01 * d12), 0.0, 1e-9, "r2");

    // Covariance of a rotation with only a prior of sqrt-info 10: 0.01 I.
    const auto cov = factor_graph::jointCovariance(f, v, std::array<Key, 1>{k0});
    check(cov.has_value(), "rotation covariance exists");
    if (cov)
        for (Eigen::Index i = 0; i < 3; ++i) near((*cov)(i, i), 0.01, 1e-9, "rotation covariance diagonal");
}

// ---- the marginal prior's Jacobian ----------------------------------------

void testLinearPriorJacobian()
{
    const Key kr = symbol('r', 0), kx = symbol('x', 0);
    Values lin;
    lin.insert(kr, Rot3::from_yaw_pitch_roll(0.4, 0.3, -0.2));
    lin.insert(kx, v1(2.0));
    Eigen::MatrixXd A = Eigen::MatrixXd::Random(4, 4);
    const Eigen::MatrixXd H = A * A.transpose() + Eigen::MatrixXd::Identity(4, 4);
    const Eigen::VectorXd g = Eigen::VectorXd::Random(4);
    const auto prior = factor_graph::LinearPrior::fromHessian({kr, kx}, lin, H, g);
    check(prior != nullptr, "prior built");
    if (!prior) return;
    check(prior->dim() == 4, "full-rank prior keeps every direction");

    // At its linearisation point the prior's gradient is g and its Hessian H.
    const auto l0 = prior->linearize(lin);
    Eigen::MatrixXd J(4, 4);
    J << l0.jacobians[0], l0.jacobians[1];
    check((J.transpose() * J - H).cwiseAbs().maxCoeff() < 1e-9, "R^T R = H");
    check((J.transpose() * l0.residual - g).cwiseAbs().maxCoeff() < 1e-9, "R^T e = g");

    // Away from it, the Jacobian must match finite differences taken through
    // the retraction -- this is the local-coordinates Jacobian csym derives.
    Values away = lin;
    away.variable(kr).retract(std::array<double, 3>{0.3, -0.5, 0.2});
    away.variable(kx).retract(std::array<double, 1>{1.5});
    const auto la = prior->linearize(away);
    const double h = 1e-7;
    for (std::size_t var = 0; var < 2; ++var)
    {
        const Key k = var == 0 ? kr : kx;
        const std::size_t n = var == 0 ? 3 : 1;
        for (std::size_t c = 0; c < n; ++c)
        {
            Values p = away, m = away;
            std::array<double, 3> dp{}, dm{};
            dp[c] = h;
            dm[c] = -h;
            p.variable(k).retract(std::span<const double>(dp.data(), n));
            m.variable(k).retract(std::span<const double>(dm.data(), n));
            const Eigen::VectorXd fd = (prior->residual(p) - prior->residual(m)) / (2 * h);
            const Eigen::VectorXd an = la.jacobians[var].col(static_cast<Eigen::Index>(c));
            check((fd - an).cwiseAbs().maxCoeff() < 1e-6,
                  fmt::format("prior Jacobian var {} col {}: diff {:.3g}", var, c, (fd - an).cwiseAbs().maxCoeff()));
        }
    }

    // A rank-deficient H keeps only the informative directions.
    Eigen::MatrixXd Hd = Eigen::MatrixXd::Zero(4, 4);
    Hd(3, 3) = 4.0;
    const auto thin = factor_graph::LinearPrior::fromHessian({kr, kx}, lin, Hd, Eigen::VectorXd::Zero(4));
    check(thin && thin->dim() == 1, "rank-1 prior has one row");
    check(factor_graph::LinearPrior::fromHessian({kr, kx}, lin, Eigen::MatrixXd::Zero(4, 4),
                                                 Eigen::VectorXd::Zero(4)) == nullptr,
          "an all-zero prior is nothing");
}

// ---- step control -----------------------------------------------------------

void testRosenbrock()
{
    FactorList f;
    f.push_back(std::make_shared<RosenbrockFactor>("rosenbrock", std::array<Key, 2>{symbol('a', 0), symbol('b', 0)}));
    Values v;
    v.insert(symbol('a', 0), v1(-1.2));
    v.insert(symbol('b', 0), v1(1.0));
    factor_graph::LmParams params;
    params.max_iterations = 200;
    const auto report = factor_graph::optimize(f, v, params);
    check(report.converged, "rosenbrock converged: " + report.stop_reason);
    check(report.final_cost <= report.initial_cost, "cost never rises");
    near(v.at<V1>(symbol('a', 0))[0], 1.0, 1e-8, "rosenbrock x");
    near(v.at<V1>(symbol('b', 0))[0], 1.0, 1e-8, "rosenbrock y");
    SPDLOG_INFO("rosenbrock: {} iterations, {} rejected", report.iterations, report.rejected_steps);
}

// ---- a sparsity pattern that changes ------------------------------------------

// r = x_k x_(k+1) - c: at x = 0 every coupling Jacobian entry is exactly zero,
// so the first normal equations are diagonal and the second are not. A solver
// that analysed the first pattern and refactorised the second into it wrote
// past the end of its own arrays -- a heap corruption that only showed up as
// a crash somewhere else, minutes into a drive.
constexpr auto kBilinear = [](auto a, auto b, auto c, auto sigma) { return (a * b[0] - c) / sigma; };
using BilinearFactor =
    factor_graph::CsymFactor<kBilinear, factor_graph::Vars<V1, V1>, factor_graph::Params<V1, double>>;

// A chain whose coupling Jacobians are exactly zero at the start point
// (d(a b)/da = b = 0) and not one step later. Once, that changed the sparsity
// pattern between iterations, and an analysis reused from the first wrote
// past the end of the factor. Solved through one SolverCache: the pattern must
// stay put across iterations (so the analysis is reused), and a problem of
// the same size with a different pattern must get a new one -- and the right
// answer.
void testChangingPattern()
{
    constexpr std::uint64_t n = 40;
    const auto chain = [&](std::uint64_t stride, FactorList& f, Values& v) {
        for (std::uint64_t k = 0; k < n; ++k)
        {
            const Key key = symbol('x', k);
            v.insert(key, v1(0.0));
            // Priors pull each variable to 1.5 + a little; the bilinear terms
            // want products of 2.
            f.push_back(std::make_shared<ScalarPriorFactor>("prior", std::array<Key, 1>{key},
                                                            v1(1.5 + 0.01 * static_cast<double>(k)), 1.0));
            if (k >= stride)
                f.push_back(std::make_shared<BilinearFactor>(
                    "bilinear", std::array<Key, 2>{symbol('x', k - stride), key}, v1(2.0), 0.1));
        }
    };
    // At the minimum the gradient J^T r is zero.
    const auto gradient = [&](const FactorList& f, const Values& v) {
        Eigen::VectorXd g = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(n));
        for (const auto& factor : f)
        {
            const auto lin = factor->linearize(v);
            const auto keys = factor->keys();
            for (std::size_t a = 0; a < keys.size(); ++a)
                g[static_cast<Eigen::Index>(factor_graph::symbolIndex(keys[a]))] +=
                    (lin.jacobians[a].transpose() * lin.residual)(0);
        }
        return g.cwiseAbs().maxCoeff();
    };

    factor_graph::SolverCache cache;
    FactorList f;
    Values v;
    chain(1, f, v);
    const auto report = factor_graph::optimize(f, v, {}, &cache);
    check(report.converged, "changing pattern: converged, " + report.stop_reason);
    check(gradient(f, v) < 1e-6, fmt::format("changing pattern: gradient {:.3g} at the answer", gradient(f, v)));
    check(cache.analyses() == 1 && cache.reuses() + 1 >= static_cast<std::uint64_t>(report.iterations),
          fmt::format("one analysis for every iteration ({} analyses, {} reuses, {} iterations)", cache.analyses(),
                      cache.reuses(), report.iterations));

    // Same size AND the same number of non-zeros, in different places: only
    // a comparison of the indices themselves tells the patterns apart.
    FactorList f2;
    Values v2;
    chain(2, f2, v2);
    f2.push_back(std::make_shared<BilinearFactor>("bilinear", std::array<Key, 2>{symbol('x', 0), symbol('x', n - 1)},
                                                  v1(2.0), 0.1));
    const auto report2 = factor_graph::optimize(f2, v2, {}, &cache);
    check(report2.converged && gradient(f2, v2) < 1e-6, "a new pattern of the same size is solved right");
    check(cache.analyses() == 2, "and gets an analysis of its own");
}

// ---- what cannot be observed -------------------------------------------------

void testUnobservable()
{
    // Two variables constrained only in their difference: the sum is free.
    FactorList f;
    f.push_back(std::make_shared<DifferenceFactor>("diff", std::array<Key, 2>{symbol('a', 0), symbol('b', 0)}, v1(3.0),
                                                   0.1));
    Values v;
    v.insert(symbol('a', 0), v1(1.0));
    v.insert(symbol('b', 0), v1(1.0));
    const auto report = factor_graph::optimize(f, v, {});
    check(report.converged, "gauge-free problem still converges: " + report.stop_reason);
    const double a = v.at<V1>(symbol('a', 0))[0], b = v.at<V1>(symbol('b', 0))[0];
    check(std::isfinite(a) && std::isfinite(b), "no NaN from a singular system");
    near(b - a, 3.0, 1e-9, "the observable difference is solved");
    check(!factor_graph::jointCovariance(f, v, std::array<Key, 1>{symbol('a', 0)}).has_value(),
          "covariance of an unobservable variable is reported as unknown");

    // A variable no factor touches at all.
    Values lonely;
    lonely.insert(symbol('z', 0), v1(5.0));
    FactorList none;
    factor_graph::optimize(none, lonely, {});
    near(lonely.at<V1>(symbol('z', 0))[0], 5.0, 0.0, "an unconstrained variable does not move");
}

// Marginalisation's careful paths. A full-rank block goes through Cholesky;
// only a singular one reaches the pseudo-inverse and the eigendecomposition,
// so nothing else in the suite exercises them. Linear, so the answers are
// exact: a0 ~ N(1, 1), x1 - a0 ~ N(0, 1) gives x1 ~ N(1, 2).
void testRankDeficientMarginalisation()
{
    const auto run = [](bool singular_blanket) {
        factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 0.5, .lm = {}, .rank_tolerance = 1e-12});
        const Key a0 = symbol('a', 0), c0 = symbol('c', 0), x1 = symbol('x', 1), w1 = symbol('w', 1);
        Values v0;
        v0.insert(a0, v1(0.0));
        v0.insert(c0, v1(0.0));
        FactorList f0;
        f0.push_back(std::make_shared<ScalarPriorFactor>("a0", std::array<Key, 1>{a0}, v1(1.0), 1.0));
        check(fls.update(f0, v0, {{a0, 0.0}, {c0, 0.0}}).ok, "first update");

        Values v1s;
        v1s.insert(x1, v1(0.0));
        v1s.insert(w1, v1(0.0));
        FactorList f1;
        f1.push_back(std::make_shared<DifferenceFactor>("x1 - a0", std::array<Key, 2>{a0, x1}, v1(0.0), 1.0));
        // Without it c0 is touched by nothing: marginalising it inverts a
        // singular block (the pseudo-inverse). With it, c0 carries exactly
        // w1's information away, and the prior left on w1 is rank-deficient
        // (the eigendecomposition).
        if (singular_blanket)
            f1.push_back(std::make_shared<DifferenceFactor>("w1 - c0", std::array<Key, 2>{c0, w1}, v1(0.0), 1.0));
        f1.push_back(std::make_shared<ScalarPriorFactor>("w1", std::array<Key, 1>{w1}, v1(5.0), 2.0));
        check(fls.update(f1, v1s, {{x1, 1.0}, {w1, 1.0}}).ok, "second update");
        check(!fls.estimate().contains(a0) && !fls.estimate().contains(c0), "the first step was marginalised");

        const std::string label = singular_blanket ? "rank-deficient prior" : "singular marginal block";
        near(fls.estimate().at<V1>(x1)[0], 1.0, 1e-9, label + ": x1's mean");
        near(fls.estimate().at<V1>(w1)[0], 5.0, 1e-9, label + ": w1's mean, untouched");
        const auto cx = fls.covariance(x1), cw = fls.covariance(w1);
        check(cx && cw, label + ": both still have a covariance");
        if (cx) near((*cx)(0, 0), 2.0, 1e-9, label + ": x1's variance");
        if (cw) near((*cw)(0, 0), 4.0, 1e-9, label + ": w1's variance, nothing added by c0");
    };
    run(false);
    run(true);

    // And the prior itself, off the optimum where its gradient is not zero:
    // H of rank one, g in its range. R^T R = H and R^T e = g, with nothing
    // claimed about the null direction.
    Values at;
    at.insert(symbol('p', 0), v1(0.0));
    at.insert(symbol('q', 0), v1(0.0));
    const std::vector<Key> keys{symbol('p', 0), symbol('q', 0)};
    const Eigen::Matrix2d H = (Eigen::Matrix2d() << 4.0, 2.0, 2.0, 1.0).finished();
    const Eigen::Vector2d g(6.0, 3.0);
    const auto prior = factor_graph::LinearPrior::fromHessian(keys, at, H, g, 1e-12);
    check(prior != nullptr, "a rank-one Hessian still makes a prior");
    if (prior)
    {
        const auto lin = prior->linearize(at);
        Eigen::MatrixXd R(lin.residual.size(), 2);
        R << lin.jacobians[0], lin.jacobians[1];
        check(lin.residual.size() == 1, "one row: the null direction claims nothing");
        check((R.transpose() * R - H).norm() < 1e-12, "R^T R is the Hessian");
        check((R.transpose() * lin.residual - g).norm() < 1e-12, "R^T e is the gradient");
    }
}

// ---- static variables ------------------------------------------------------

void testWidelyScaledInformation()
{
    // A chain y known only weakly in absolute terms (a 300 m prior and a
    // loose walk, like a barometric offset) beside a chain z that is stiff
    // (1e-5 sigmas, like a gyro-bias walk over 0.1 s): fifteen orders of
    // magnitude of information in one window. Marginalised every step, y's prior must survive into the
    // marginal prior and its covariance must still be answerable. Judged
    // against the stiffest thing in the window, y's information was below any
    // relative tolerance and was dropped: y became unobservable.
    const double weak = 300.0, loose = 0.08, stiff = 1e-5;
    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 0.0, .lm = {}, .rank_tolerance = 1e-12});
    std::optional<Eigen::MatrixXd> cov;
    for (std::size_t k = 0; k < 20; ++k)
    {
        const Key y = symbol('y', k), z = symbol('z', k);
        Values v;
        v.insert(y, v1(0.0));
        v.insert(z, v1(0.0));
        FactorList f;
        f.push_back(std::make_shared<ScalarPriorFactor>("z", std::array<Key, 1>{z}, v1(0.0), stiff));
        if (k == 0)
        {
            f.push_back(std::make_shared<ScalarPriorFactor>("y0 prior", std::array<Key, 1>{y}, v1(0.0), weak));
        }
        else
        {
            f.push_back(std::make_shared<DifferenceFactor>("y walk", std::array<Key, 2>{symbol('y', k - 1), y}, v1(0.0),
                                                           loose));
            f.push_back(std::make_shared<DifferenceFactor>("z walk", std::array<Key, 2>{symbol('z', k - 1), z}, v1(0.0),
                                                           stiff));
        }
        const auto report = fls.update(f, v, {{y, static_cast<double>(k)}, {z, static_cast<double>(k)}});
        check(report.ok, "update " + std::to_string(k));
        cov = fls.covariance(y);
    }
    check(cov.has_value(), "the weakly known chain is still observable after twenty marginalisations");
    if (cov)
        near(std::sqrt((*cov)(0, 0)), std::sqrt(weak * weak + 19.0 * loose * loose), 1e-3 * weak,
             "and its sigma is still the 300 m it was given, plus its walk");
}

void testStaticVariable()
{
    // A random walk x_k seen through a constant sensor bias b: z_k = x_k + b.
    // Only a prior on x0 and on b makes b observable at all, and the lag-0
    // filter must end with exactly the batch answer for b, because
    // marginalising everything but b and the newest x is exact here.
    std::mt19937 rng(7);
    std::normal_distribution<double> n(0.0, 1.0);
    const std::size_t steps = 60;
    std::vector<double> z;
    double x = 0.0;
    for (std::size_t k = 0; k < steps; ++k)
    {
        x += 0.2 * n(rng);
        z.push_back(x + 0.7 + 0.1 * n(rng));
    }

    const Key kb = symbol('b', 0);
    const auto factorsAt = [&](std::size_t k) {
        FactorList f;
        if (k == 0)
        {
            f.push_back(std::make_shared<ScalarPriorFactor>("x0 prior", std::array<Key, 1>{symbol('x', 0)}, v1(0.0), 0.5));
            f.push_back(std::make_shared<ScalarPriorFactor>("bias prior", std::array<Key, 1>{kb}, v1(0.0), 1.0));
        }
        else
        {
            f.push_back(std::make_shared<DifferenceFactor>("walk", std::array<Key, 2>{symbol('x', k - 1), symbol('x', k)},
                                                           v1(0.0), 0.2));
        }
        f.push_back(std::make_shared<BiasedFactor>("z", std::array<Key, 2>{symbol('x', k), kb}, v1(z[k]), 0.1));
        return f;
    };

    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 0.0, .lm = {}, .rank_tolerance = 1e-12});
    factor_graph::BatchSmoother batch;
    double previous_var = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < steps; ++k)
    {
        Values v;
        v.insert(symbol('x', k), v1(0.0));
        std::map<Key, double> stamps{{symbol('x', k), static_cast<double>(k)}};
        if (k == 0)
        {
            v.insert(kb, v1(0.0));
            stamps[kb] = factor_graph::kStatic;
        }
        check(fls.update(factorsAt(k), v, stamps).ok, "static test update");
        check(batch.add(factorsAt(k), v).ok, "static test batch add");
        check(fls.estimate().contains(kb), fmt::format("bias still live at step {}", k));
        check(fls.estimate().size() == 2, fmt::format("lag 0 keeps newest x and the bias at step {}", k));
        if (const auto cov = fls.covariance(kb))
        {
            // More data never loses information about b. Relative slack for
            // the rounding in two different factorisations of the same number.
            check((*cov)(0, 0) <= previous_var * (1.0 + 1e-9),
                  fmt::format("bias uncertainty never grows: {:.17g} after {:.17g} at step {}", (*cov)(0, 0),
                              previous_var, k));
            previous_var = (*cov)(0, 0);
        }
    }
    batch.optimize();
    near(fls.estimate().at<V1>(kb)[0], batch.estimate().at<V1>(kb)[0], 1e-9, "filtered bias = batch bias");
    const auto cf = fls.covariance(kb), cb = batch.covariance(kb);
    check(cf && cb, "bias covariance available");
    if (cf && cb) near((*cf)(0, 0), (*cb)(0, 0), 1e-12, "filtered bias variance = batch");
}

// ---- refusals ------------------------------------------------------------------

// A factor that can be made to misbehave in each of the ways the checks catch.
class BrokenFactor final : public factor_graph::Factor
{
  public:
    enum class Fault
    {
        none,
        no_keys,
        short_residual,
        wrong_jacobian_cols,
        nan_residual,
        throws,
    };

    BrokenFactor(std::vector<Key> keys, Fault fault) : Factor(std::move(keys)), fault_(fault) {}

    std::size_t dim() const override { return 2; }
    std::string name() const override { return "broken"; }

    factor_graph::Linearization linearize(const Values& values) const override
    {
        factor_graph::Linearization l;
        l.residual = residual(values);
        for (Key k : keys())
        {
            auto cols = static_cast<Eigen::Index>(values.variable(k).tangentDim());
            if (fault_ == Fault::wrong_jacobian_cols) cols += 1;
            l.jacobians.push_back(Eigen::MatrixXd::Ones(2, cols));
        }
        return l;
    }

    Eigen::VectorXd residual(const Values&) const override
    {
        switch (fault_)
        {
            case Fault::throws:
                throw std::runtime_error("sensor exploded");
            case Fault::short_residual:
                return Eigen::VectorXd::Zero(1);
            case Fault::nan_residual:
                return Eigen::VectorXd::Constant(2, std::numeric_limits<double>::quiet_NaN());
            case Fault::none:
            case Fault::no_keys:
            case Fault::wrong_jacobian_cols:
                break;
        }
        return Eigen::VectorXd::Zero(2);
    }

  private:
    Fault fault_;
};

void testRefusals()
{
    using Fault = BrokenFactor::Fault;
    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 5.0, .lm = {}, .rank_tolerance = 1e-12});
    const Key a = symbol('a', 0), b = symbol('b', 0);
    {
        Values v;
        v.insert(a, v1(1.0));
        FactorList f{std::make_shared<ScalarPriorFactor>("a prior", std::array<Key, 1>{a}, v1(1.0), 1.0)};
        check(fls.update(f, v, {{a, 0.0}}).ok, "a good first update");
    }
    const double a_before = fls.estimate().at<V1>(a)[0];
    const std::size_t factors_before = fls.factors().size();

    const auto refuse = [&](const FactorList& f, const Values& v, const std::map<Key, double>& stamps,
                            const std::string& expect, const std::string& what) {
        const auto r = fls.update(f, v, stamps);
        check(!r.ok, what + " is refused");
        check(r.error.find(expect) != std::string::npos,
              fmt::format("{}: error '{}' should mention '{}'", what, r.error, expect));
        check(fls.estimate().size() == 1 && fls.factors().size() == factors_before &&
                  fls.estimate().at<V1>(a)[0] == a_before,
              what + " leaves the state untouched");
    };

    Values vb;
    vb.insert(b, v1(0.0));
    const std::map<Key, double> sb{{b, 1.0}};
    const auto broken = [](std::vector<Key> keys, Fault fault) {
        return FactorList{std::make_shared<BrokenFactor>(std::move(keys), fault)};
    };

    refuse(broken({}, Fault::no_keys), vb, sb, "no variables", "a factor with no variables");
    refuse(broken({a, a}, Fault::none), vb, sb, "appears twice", "a factor naming one variable twice");
    refuse(broken({symbol('q', 9)}, Fault::none), vb, sb, "unknown variable", "a factor on an unknown variable");
    refuse(broken({b}, Fault::short_residual), vb, sb, "residual has", "a residual of the wrong length");
    refuse(broken({b}, Fault::wrong_jacobian_cols), vb, sb, "Jacobian", "a Jacobian of the wrong shape");
    refuse(broken({b}, Fault::nan_residual), vb, sb, "not finite", "a NaN residual");
    refuse(broken({b}, Fault::throws), vb, sb, "sensor exploded", "a factor that throws");
    refuse(FactorList{nullptr}, vb, sb, "null factor", "a null factor");

    const FactorList ok_b{std::make_shared<ScalarPriorFactor>("b prior", std::array<Key, 1>{b}, v1(0.0), 1.0)};
    refuse(ok_b, vb, {}, "no stamp", "a new variable without a stamp");
    refuse(ok_b, vb, {{b, std::numeric_limits<double>::quiet_NaN()}}, "invalid stamp", "a NaN stamp");
    refuse(ok_b, vb, {{b, 1.0}, {symbol('c', 0), 1.0}}, "not a new variable", "a stamp for nothing");

    Values dup;
    dup.insert(a, v1(0.0));
    refuse(FactorList{}, dup, {{a, 1.0}}, "already exists", "re-adding an existing variable");

    Values bad;
    bad.insert(b, v1(std::numeric_limits<double>::infinity()));
    refuse(ok_b, bad, sb, "not finite", "an infinite initial value");

    // A CsymFactor whose measurement is NaN is caught the same way.
    const FactorList nan_measure{std::make_shared<ScalarPriorFactor>(
        "nan prior", std::array<Key, 1>{b}, v1(std::numeric_limits<double>::quiet_NaN()), 1.0)};
    refuse(nan_measure, vb, sb, "not finite", "a NaN measurement");

    // And after all that, a good update still works.
    check(fls.update(ok_b, vb, sb).ok, "the smoother still accepts a good update");
}

}  // namespace

int main()
{
    testRotations();
    testLinearPriorJacobian();
    testRosenbrock();
    testChangingPattern();
    testUnobservable();
    testWidelyScaledInformation();
    testRankDeficientMarginalisation();
    testStaticVariable();
    testRefusals();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("graph: all passed");
    return 0;
}
