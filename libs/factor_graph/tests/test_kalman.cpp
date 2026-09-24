// SPDX-License-Identifier: GPL-3.0-or-later
//
// The smoothers against the two estimators they generalise.
//
// On a linear-Gaussian model every step the smoothers take is exact --
// Gauss-Newton converges in one iteration, and a Schur complement is exactly
// the marginal -- so there is a closed-form answer to compare with, to
// rounding:
//
//   * FixedLagSmoother with lag 0 is a Kalman filter: after each update its
//     one live state and covariance are the filtered mean and P(k|k).
//   * FixedLagSmoother with lag L holds, at step k, the RTS smoother's answer
//     over the data up to k for the last L steps.
//   * BatchSmoother is the RTS smoother: every state, every covariance.
//
// The filter and smoother below are the textbook recursions, written against
// Eigen with nothing shared with the graph.

#include "factor_graph/csym_factor.h"
#include "factor_graph/smoother.h"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

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

void near(const Eigen::MatrixXd& got, const Eigen::MatrixXd& want, double tolerance, const std::string& what)
{
    const double err = (got - want).cwiseAbs().maxCoeff();
    const double scale = std::max(1.0, want.cwiseAbs().maxCoeff());
    check(err <= tolerance * scale, fmt::format("{}: max error {:.3g} (scale {:.3g})", what, err, scale));
}

using factor_graph::Key;
using factor_graph::symbol;

template <std::size_t R, std::size_t C>
csym::Matrix<double, R, C> fromEigen(const Eigen::MatrixXd& m)
{
    csym::Matrix<double, R, C> out;
    for (std::size_t r = 0; r < R; ++r)
        for (std::size_t c = 0; c < C; ++c) out(r, c) = m(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
    return out;
}

template <std::size_t R>
Eigen::VectorXd toEigen(const csym::Matrix<double, R, 1>& v)
{
    Eigen::VectorXd out(static_cast<Eigen::Index>(R));
    for (std::size_t i = 0; i < R; ++i) out[static_cast<Eigen::Index>(i)] = v[i];
    return out;
}

// sqrt information: S with S^T S = P^-1.
Eigen::MatrixXd sqrtInfo(const Eigen::MatrixXd& cov)
{
    const Eigen::MatrixXd info = cov.inverse();
    return Eigen::LLT<Eigen::MatrixXd>(info).matrixU();
}

// ---- the model, as factors -----------------------------------------------

constexpr auto kPrior = [](auto x, auto mean, auto sqrt_info) { return sqrt_info * (x - mean); };
constexpr auto kProcess = [](auto x0, auto x1, auto f, auto sqrt_info) { return sqrt_info * (x1 - f * x0); };
constexpr auto kMeasure = [](auto x, auto z, auto h, auto sqrt_info) { return sqrt_info * (z - h * x); };

template <std::size_t N, std::size_t M>
struct Model
{
    using V = csym::Vector<double, N>;
    using PriorF = factor_graph::CsymFactor<kPrior, factor_graph::Vars<V>, factor_graph::Params<V, csym::Matrix<double, N, N>>>;
    using ProcessF = factor_graph::CsymFactor<kProcess, factor_graph::Vars<V, V>,
                                              factor_graph::Params<csym::Matrix<double, N, N>, csym::Matrix<double, N, N>>>;
    using MeasureF = factor_graph::CsymFactor<kMeasure, factor_graph::Vars<V>,
                                              factor_graph::Params<csym::Vector<double, M>, csym::Matrix<double, M, N>,
                                                                   csym::Matrix<double, M, M>>>;

    Eigen::MatrixXd F, Q, H, R, P0;
    Eigen::VectorXd m0;
    std::vector<Eigen::VectorXd> z;  // empty vector = no measurement at that step

    std::shared_ptr<const factor_graph::Factor> prior() const
    {
        return std::make_shared<PriorF>("prior", std::array<Key, 1>{symbol('x', 0)}, fromEigen<N, 1>(m0),
                                        fromEigen<N, N>(sqrtInfo(P0)));
    }
    std::shared_ptr<const factor_graph::Factor> process(std::size_t k) const
    {
        return std::make_shared<ProcessF>("process", std::array<Key, 2>{symbol('x', k - 1), symbol('x', k)},
                                          fromEigen<N, N>(F), fromEigen<N, N>(sqrtInfo(Q)));
    }
    std::shared_ptr<const factor_graph::Factor> measure(std::size_t k) const
    {
        return std::make_shared<MeasureF>("measure", std::array<Key, 1>{symbol('x', k)}, fromEigen<M, 1>(z[k]),
                                          fromEigen<M, N>(H), fromEigen<M, M>(sqrtInfo(R)));
    }

    // Everything added at step k.
    factor_graph::FactorList factorsAt(std::size_t k) const
    {
        factor_graph::FactorList out;
        out.push_back(k == 0 ? prior() : process(k));
        if (z[k].size()) out.push_back(measure(k));
        return out;
    }
};

// ---- the oracle ----------------------------------------------------------

struct KalmanRun
{
    std::vector<Eigen::VectorXd> x_pred, x_filt, x_smooth;
    std::vector<Eigen::MatrixXd> p_pred, p_filt, p_smooth;
};

template <std::size_t N, std::size_t M>
KalmanRun kalmanRts(const Model<N, M>& m, std::size_t steps)
{
    KalmanRun run;
    const auto n = static_cast<Eigen::Index>(N);
    for (std::size_t k = 0; k < steps; ++k)
    {
        Eigen::VectorXd x = k == 0 ? m.m0 : Eigen::VectorXd(m.F * run.x_filt.back());
        Eigen::MatrixXd P = k == 0 ? m.P0 : Eigen::MatrixXd(m.F * run.p_filt.back() * m.F.transpose() + m.Q);
        run.x_pred.push_back(x);
        run.p_pred.push_back(P);
        if (m.z[k].size())
        {
            const Eigen::MatrixXd S = m.H * P * m.H.transpose() + m.R;
            const Eigen::MatrixXd K = P * m.H.transpose() * S.inverse();
            x = x + K * (m.z[k] - m.H * x);
            const Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(n, n) - K * m.H;
            P = IKH * P * IKH.transpose() + K * m.R * K.transpose();  // Joseph form
        }
        run.x_filt.push_back(x);
        run.p_filt.push_back(P);
    }
    run.x_smooth = run.x_filt;
    run.p_smooth = run.p_filt;
    for (std::size_t k = steps - 1; k-- > 0;)
    {
        const Eigen::MatrixXd C = run.p_filt[k] * m.F.transpose() * run.p_pred[k + 1].inverse();
        run.x_smooth[k] = run.x_filt[k] + C * (run.x_smooth[k + 1] - run.x_pred[k + 1]);
        run.p_smooth[k] = run.p_filt[k] + C * (run.p_smooth[k + 1] - run.p_pred[k + 1]) * C.transpose();
    }
    return run;
}

// ---- the models ------------------------------------------------------------

Model<1, 1> randomWalk(std::size_t steps, std::mt19937& rng)
{
    Model<1, 1> m;
    m.F = Eigen::MatrixXd::Constant(1, 1, 0.95);
    m.Q = Eigen::MatrixXd::Constant(1, 1, 0.3);
    m.H = Eigen::MatrixXd::Constant(1, 1, 1.0);
    m.R = Eigen::MatrixXd::Constant(1, 1, 2.0);
    m.P0 = Eigen::MatrixXd::Constant(1, 1, 10.0);
    m.m0 = Eigen::VectorXd::Constant(1, 3.0);
    std::normal_distribution<double> n(0.0, 1.0);
    double x = 5.0;
    for (std::size_t k = 0; k < steps; ++k)
    {
        x = 0.95 * x + std::sqrt(0.3) * n(rng);
        // Every fifth step has no measurement: prediction-only steps are
        // where a filter and a smoother most visibly differ.
        m.z.push_back(k % 5 == 3 ? Eigen::VectorXd() : Eigen::VectorXd::Constant(1, x + std::sqrt(2.0) * n(rng)));
    }
    return m;
}

// Constant velocity in 3-D, position measured: a 6-state model whose
// covariances couple every state to every other.
Model<6, 3> constantVelocity(std::size_t steps, std::mt19937& rng)
{
    Model<6, 3> m;
    const double dt = 0.1;
    m.F = Eigen::MatrixXd::Identity(6, 6);
    m.F.topRightCorner(3, 3) = dt * Eigen::MatrixXd::Identity(3, 3);
    // Discrete white-noise acceleration, q = 0.5 (m/s^2)^2/Hz.
    const double q = 0.5;
    m.Q = Eigen::MatrixXd::Zero(6, 6);
    m.Q.topLeftCorner(3, 3) = q * dt * dt * dt / 3.0 * Eigen::MatrixXd::Identity(3, 3);
    m.Q.topRightCorner(3, 3) = q * dt * dt / 2.0 * Eigen::MatrixXd::Identity(3, 3);
    m.Q.bottomLeftCorner(3, 3) = m.Q.topRightCorner(3, 3);
    m.Q.bottomRightCorner(3, 3) = q * dt * Eigen::MatrixXd::Identity(3, 3);
    m.H = Eigen::MatrixXd::Zero(3, 6);
    m.H.leftCols(3) = Eigen::MatrixXd::Identity(3, 3);
    m.R = Eigen::MatrixXd::Identity(3, 3) * 0.04;
    m.R(0, 1) = m.R(1, 0) = 0.01;
    m.P0 = Eigen::MatrixXd::Identity(6, 6) * 25.0;
    m.m0 = Eigen::VectorXd::Zero(6);
    std::normal_distribution<double> n(0.0, 1.0);
    Eigen::VectorXd x(6);
    x << 100.0, -40.0, 3.0, 12.0, 5.0, -0.5;
    const Eigen::MatrixXd lq = Eigen::LLT<Eigen::MatrixXd>(m.Q).matrixL();
    const Eigen::MatrixXd lr = Eigen::LLT<Eigen::MatrixXd>(m.R).matrixL();
    for (std::size_t k = 0; k < steps; ++k)
    {
        Eigen::VectorXd w(6), v(3);
        for (auto& e : w) e = n(rng);
        for (auto& e : v) e = n(rng);
        x = m.F * x + lq * w;
        m.z.push_back(k % 7 == 5 ? Eigen::VectorXd() : Eigen::VectorXd(m.H * x + lr * v));
    }
    return m;
}

// ---- the tests -------------------------------------------------------------

// A deliberately bad initial guess for each new state, so the solve has
// work to do: a smoother that only reproduced its initial values would pass
// a test seeded with the right answer.
template <std::size_t N>
csym::Vector<double, N> badGuess()
{
    csym::Vector<double, N> v;
    for (std::size_t i = 0; i < N; ++i) v[i] = 1000.0 + static_cast<double>(i);
    return v;
}

template <std::size_t N, std::size_t M>
void testFilter(const Model<N, M>& m, std::size_t steps, const std::string& label)
{
    const KalmanRun kf = kalmanRts(m, steps);
    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 0.0, .lm = {}, .rank_tolerance = 1e-12});
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        const auto report = fls.update(m.factorsAt(k), v, {{symbol('x', k), static_cast<double>(k)}});
        check(report.ok, label + " update: " + report.error);
        check(fls.estimate().size() == 1, fmt::format("{} step {}: lag 0 keeps one state, has {}", label, k,
                                                      fls.estimate().size()));
        const auto x = toEigen(fls.estimate().at<csym::Vector<double, N>>(symbol('x', k)));
        near(x, kf.x_filt[k], 1e-9, fmt::format("{} filtered mean at {}", label, k));
        const auto p = fls.covariance(symbol('x', k));
        check(p.has_value(), fmt::format("{} covariance at {}", label, k));
        if (p) near(*p, kf.p_filt[k], 1e-9, fmt::format("{} P(k|k) at {}", label, k));
    }
}

// The covariance update() hands back, taken from the solve's own
// factorisation when the solve ended undamped on a negligible step. On a
// linear model that factorisation IS the Hessian, so it must match the
// filter to rounding -- and it must actually have been used, or the check
// would pass on the fallback alone.
template <std::size_t N, std::size_t M>
void testCovarianceFromSolve(const Model<N, M>& m, std::size_t steps, const std::string& label)
{
    const KalmanRun kf = kalmanRts(m, steps);
    factor_graph::LmParams lm;
    lm.initial_lambda = 1e-14;
    lm.step_tolerance = 1e-6;  // above rounding, so every solve ends on the step inside it
    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = 2.0, .lm = lm, .rank_tolerance = 1e-12});
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        const std::array<Key, 1> want{symbol('x', k)};
        const auto report = fls.update(m.factorsAt(k), v, {{symbol('x', k), static_cast<double>(k)}}, want);
        check(report.ok && report.covariance.has_value(), fmt::format("{} step {}: a covariance", label, k));
        if (report.covariance) near(*report.covariance, kf.p_filt[k], 1e-9, fmt::format("{} P(k|k) from the solve at {}", label, k));
    }
    check(fls.solverCache().covariancesFromSolve() == steps,
          fmt::format("{}: every covariance came from the solve ({} of {})", label,
                      fls.solverCache().covariancesFromSolve(), steps));

    // Damped as a default LmParams leaves it, the solve's factorisation is
    // not the Hessian's: the long way is taken, and gives the same answer.
    factor_graph::FixedLagSmoother damped(factor_graph::FixedLagParams{.lag = 2.0, .lm = {}, .rank_tolerance = 1e-12});
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        const std::array<Key, 1> want{symbol('x', k)};
        const auto report = damped.update(m.factorsAt(k), v, {{symbol('x', k), static_cast<double>(k)}}, want);
        if (report.covariance) near(*report.covariance, kf.p_filt[k], 1e-9, fmt::format("{} damped P(k|k) at {}", label, k));
    }
    check(damped.solverCache().covariancesFromSolve() == 0, label + ": a damped solve's factorisation is not reused");
}

template <std::size_t N, std::size_t M>
void testFixedLag(const Model<N, M>& m, std::size_t steps, double lag, const std::string& label)
{
    factor_graph::FixedLagSmoother fls(factor_graph::FixedLagParams{.lag = lag, .lm = {}, .rank_tolerance = 1e-12});
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        const auto report = fls.update(m.factorsAt(k), v, {{symbol('x', k), static_cast<double>(k)}});
        check(report.ok, label + " update: " + report.error);

        // Everything still in the window is the RTS answer on data up to k.
        const KalmanRun rts = kalmanRts(m, k + 1);
        std::size_t live = 0;
        for (std::size_t j = 0; j <= k; ++j)
        {
            if (!fls.estimate().contains(symbol('x', j))) continue;
            ++live;
            near(toEigen(fls.estimate().at<csym::Vector<double, N>>(symbol('x', j))), rts.x_smooth[j], 1e-9,
                 fmt::format("{} step {}: x{} vs RTS over 0..{}", label, k, j, k));
            if (const auto p = fls.covariance(symbol('x', j)))
                near(*p, rts.p_smooth[j], 1e-9, fmt::format("{} step {}: P{} vs RTS", label, k, j));
            else
                check(false, fmt::format("{} step {}: no covariance for x{}", label, k, j));
        }
        const std::size_t expect = std::min<std::size_t>(k + 1, static_cast<std::size_t>(lag) + 1);
        check(live == expect, fmt::format("{} step {}: {} live states, expected {}", label, k, live, expect));
    }
}

template <std::size_t N, std::size_t M>
void testBatch(const Model<N, M>& m, std::size_t steps, const std::string& label)
{
    const KalmanRun rts = kalmanRts(m, steps);
    factor_graph::BatchSmoother batch;
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        check(batch.add(m.factorsAt(k), v).ok, label + " batch add");
    }
    const auto report = batch.optimize();
    check(report.converged, label + " batch converged: " + report.stop_reason);
    for (std::size_t k = 0; k < steps; ++k)
    {
        near(toEigen(batch.estimate().at<csym::Vector<double, N>>(symbol('x', k))), rts.x_smooth[k], 1e-9,
             fmt::format("{} batch x{} vs RTS", label, k));
        const auto p = batch.covariance(symbol('x', k));
        check(p.has_value(), fmt::format("{} batch covariance x{}", label, k));
        if (p) near(*p, rts.p_smooth[k], 1e-9, fmt::format("{} batch P{} vs RTS", label, k));
    }

    // An infinite lag never marginalises, so it ends where the batch does.
    factor_graph::FixedLagSmoother forever(
        factor_graph::FixedLagParams{.lag = factor_graph::kStatic, .lm = {}, .rank_tolerance = 1e-12});
    for (std::size_t k = 0; k < steps; ++k)
    {
        factor_graph::Values v;
        v.insert(symbol('x', k), badGuess<N>());
        forever.update(m.factorsAt(k), v, {{symbol('x', k), static_cast<double>(k)}});
    }
    check(forever.estimate().size() == steps, label + " infinite lag keeps everything");
    for (std::size_t k = 0; k < steps; ++k)
        near(toEigen(forever.estimate().at<csym::Vector<double, N>>(symbol('x', k))), rts.x_smooth[k], 1e-9,
             fmt::format("{} infinite lag x{} vs RTS", label, k));
}

}  // namespace

int main()
{
    std::mt19937 rng(20260922);
    constexpr std::size_t kSteps = 40;
    const auto walk = randomWalk(kSteps, rng);
    const auto cv = constantVelocity(kSteps, rng);

    testFilter(walk, kSteps, "walk");
    testFilter(cv, kSteps, "cv");
    testCovarianceFromSolve(walk, kSteps, "walk");
    testCovarianceFromSolve(cv, kSteps, "cv");
    testFixedLag(walk, kSteps, 4.0, "walk lag 4");
    testFixedLag(cv, kSteps, 6.0, "cv lag 6");
    testBatch(walk, kSteps, "walk");
    testBatch(cv, kSteps, "cv");

    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("kalman: fixed-lag = Kalman filter and batch = RTS, to rounding");
    return 0;
}
