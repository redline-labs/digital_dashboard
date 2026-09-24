#include "factor_graph/optimizer.h"

#include <Eigen/QR>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace factor_graph
{

namespace
{

// Where each variable's tangent space sits in the stacked system.
struct Layout
{
    std::map<Key, std::pair<Eigen::Index, Eigen::Index>> at;  // offset, dim
    Eigen::Index size = 0;
};

Layout layoutOf(const Values& values)
{
    Layout l;
    for (Key k : values.keys())
    {
        const auto n = static_cast<Eigen::Index>(values.variable(k).tangentDim());
        l.at.emplace(k, std::make_pair(l.size, n));
        l.size += n;
    }
    return l;
}

struct NormalEquations
{
    Eigen::SparseMatrix<double> H;  // lower triangle only
    Eigen::VectorXd g;              // J^T r
    double cost = 0.0;
};

NormalEquations assemble(const FactorList& factors, const Values& values, const Layout& layout)
{
    NormalEquations ne;
    ne.g = Eigen::VectorXd::Zero(layout.size);
    std::vector<Eigen::Triplet<double>> triplets;
    Eigen::MatrixXd blk;
    for (const auto& f : factors)
    {
        const Linearization lin = f->linearize(values);
        ne.cost += 0.5 * lin.residual.squaredNorm();
        const auto keys = f->keys();
        for (std::size_t a = 0; a < keys.size(); ++a)
        {
            const auto [oa, na] = layout.at.at(keys[a]);
            // Blocks are a few rows by a few columns: a GEMM's packing costs
            // more than the product.
            ne.g.segment(oa, na) += lin.jacobians[a].transpose().lazyProduct(lin.residual);
            for (std::size_t b = 0; b < keys.size(); ++b)
            {
                const auto [ob, nb] = layout.at.at(keys[b]);
                if (ob > oa) continue;  // lower triangle: block row >= block column
                blk.noalias() = lin.jacobians[a].transpose().lazyProduct(lin.jacobians[b]);
                // Exact zeros are kept: a block that is zero at one
                // linearisation and not at the next would otherwise change
                // the pattern between iterations and defeat SolverCache.
                for (Eigen::Index r = 0; r < na; ++r)
                    for (Eigen::Index c = 0; c < nb; ++c)
                        if (oa + r >= ob + c) triplets.emplace_back(oa + r, ob + c, blk(r, c));
            }
        }
    }
    ne.H.resize(layout.size, layout.size);
    ne.H.setFromTriplets(triplets.begin(), triplets.end());
    ne.H.makeCompressed();
    return ne;
}

Values stepped(const Values& values, const Layout& layout, const Eigen::VectorXd& delta)
{
    Values out = values;
    for (const auto& [k, span] : layout.at)
    {
        const auto [o, n] = span;
        out.variable(k).retract(std::span<const double>(delta.data() + o, static_cast<std::size_t>(n)));
    }
    return out;
}

}  // namespace

struct SolverCache::Impl
{
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower> ldlt;
    bool analysed = false;
    Eigen::Index n = 0;
    std::vector<int> outer, inner;
    std::uint64_t analyses = 0, reuses = 0;

    // Set when a solve ended on a step inside its tolerance, undamped:
    // the factorisation then is the Hessian's at the returned values, to
    // within that step, and the covariance can come from it directly.
    bool last_usable = false;
    std::uint64_t covariances_from_solve = 0;
    Eigen::VectorXd last_scale;
    std::map<Key, std::pair<Eigen::Index, Eigen::Index>> last_layout;

    // Factorises A (compressed, lower triangle), re-analysing only when its
    // pattern is not exactly the one the analysis was made for.
    void factorize(const Eigen::SparseMatrix<double>& A)
    {
        const auto nnz = static_cast<std::size_t>(A.nonZeros());
        const auto cols = static_cast<std::size_t>(A.cols()) + 1;
        const bool same = analysed && A.rows() == n && outer.size() == cols && inner.size() == nnz &&
                          std::equal(outer.begin(), outer.end(), A.outerIndexPtr()) &&
                          std::equal(inner.begin(), inner.end(), A.innerIndexPtr());
        if (!same)
        {
            ldlt.analyzePattern(A);
            analysed = true;
            n = A.rows();
            outer.assign(A.outerIndexPtr(), A.outerIndexPtr() + cols);
            inner.assign(A.innerIndexPtr(), A.innerIndexPtr() + nnz);
            ++analyses;
        }
        else
        {
            ++reuses;
        }
        ldlt.factorize(A);
    }
};

namespace
{

// A pivot of the Jacobi-scaled Hessian below this is a direction nothing
// constrains -- or one this factorisation cannot resolve: the backward error of
// an LDLT of n unit-diagonal columns is ~n eps, about 1e-13 for a window.
constexpr double kPivotTolerance = 1e-12;
// The same test on the square-root factor, whose entries are the square roots
// of those pivots: 1e-10 here is 1e-20 of information there, and roundoff in a
// QR of unit-norm columns is ~n eps, 1e-13, far below it.
constexpr double kQrRankTolerance = 1e-10;

// s_i = 1 / sqrt(H_ii), floored: the change of variables that puts every
// diagonal at one, so a pivot is judged against its own variable's scale.
// Unscaled, a zero pivot had to be recognised against the stiffest thing in
// the window: a gyro-bias walk over 0.1 s carries 1e10 of information and a
// barometric offset known to 300 m carries 1e-5, and both are perfectly well
// determined.
Eigen::VectorXd jacobiScale(const Eigen::SparseMatrix<double>& H, double floor)
{
    return H.diagonal().cwiseMax(floor).cwiseSqrt().cwiseInverse();
}

// S H S, same pattern as H.
Eigen::SparseMatrix<double> scaled(const Eigen::SparseMatrix<double>& H, const Eigen::VectorXd& s)
{
    Eigen::SparseMatrix<double> out = H;
    for (Eigen::Index j = 0; j < out.outerSize(); ++j)
        for (Eigen::SparseMatrix<double>::InnerIterator it(out, j); it; ++it) it.valueRef() *= s[it.row()] * s[j];
    return out;
}

bool pivotsResolved(const Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower>& ldlt)
{
    return ldlt.info() == Eigen::Success && ldlt.vectorD().minCoeff() > kPivotTolerance;
}

// The joint covariance of `keys` from a factorisation of S H S, or empty if
// a pivot says the Hessian is singular or a key is not in the layout.
std::optional<Eigen::MatrixXd> blockCovariance(const Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower>& ldlt,
                                               const Eigen::VectorXd& scale,
                                               const std::map<Key, std::pair<Eigen::Index, Eigen::Index>>& layout,
                                               std::span<const Key> keys)
{
    if (ldlt.info() != Eigen::Success) return std::nullopt;
    const Eigen::VectorXd d = ldlt.vectorD();
    if (!(d.minCoeff() > kPivotTolerance)) return std::nullopt;
    Eigen::Index n = 0;
    for (Key k : keys)
    {
        const auto it = layout.find(k);
        if (it == layout.end()) return std::nullopt;
        n += it->second.second;
    }
    if (n == 0) return std::nullopt;
    // Only the block is wanted, so only a forward solve: with
    // S H S = P^T L D L^T P, the block of H^-1 = S (S H S)^-1 S selected by E
    // is Y^T D^-1 Y for Y = L^-1 P S E. The backward solve a full
    // ldlt.solve() adds was most of the covariance's time.
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(scale.size(), n);
    Eigen::Index col = 0;
    for (Key k : keys)
    {
        const auto [o, dim] = layout.at(k);
        for (Eigen::Index i = 0; i < dim; ++i) rhs(o + i, col + i) = scale[o + i];
        col += dim;
    }
    Eigen::MatrixXd y = ldlt.permutationP() * rhs;
    ldlt.matrixL().solveInPlace(y);
    const Eigen::MatrixXd cov = y.transpose() * d.cwiseInverse().asDiagonal() * y;
    if (!cov.allFinite()) return std::nullopt;
    return Eigen::MatrixXd(0.5 * (cov + cov.transpose()));
}

// The covariances again, from a QR of the whitened, column-scaled Jacobian
// instead of an LDLT of the Hessian it squares. The Hessian's condition is the
// Jacobian's squared: in a long GNSS outage each keyframe's position is held
// to its neighbours by the IMU chain at ~1e11 and to the world by what the
// marginal prior still knows, a ratio past 1e-13 once the position sigma
// passes a few metres -- below what the LDLT resolves, so the covariance
// vanished just when the output most needed it. The square root keeps half
// the exponent. Slower, so only for what the LDLT cannot answer.
//
// Dense: a window's R is more than half full whatever the column order (the
// marginal prior and the calibration couple nearly everything), and Eigen's
// blocked dense Householder does it in half the time of its SparseQR -- ~7 ms
// against 13 for a 660 x 540 window. Unpivoted: a column in the span of earlier
// ones still leaves a zero on R's diagonal, and the margin is wide -- the
// weakest real direction of a five-minute outage is ~5e-7 against
// kQrRankTolerance's 1e-10. Column pivoting cost 18 ms for nothing here.
std::vector<std::optional<Eigen::MatrixXd>> qrCovariances(const FactorList& factors, const Values& values,
                                                          const Layout& layout,
                                                          const std::vector<std::vector<Key>>& groups)
{
    std::vector<std::optional<Eigen::MatrixXd>> out(groups.size());
    Eigen::Index rows = 0;
    std::vector<Linearization> lins;
    lins.reserve(factors.size());
    for (const auto& f : factors)
    {
        lins.push_back(f->linearize(values));
        rows += lins.back().residual.size();
    }
    if (rows < layout.size) return out;  // fewer equations than unknowns: singular
    Eigen::MatrixXd J = Eigen::MatrixXd::Zero(rows, layout.size);
    Eigen::Index row = 0;
    for (std::size_t i = 0; i < factors.size(); ++i)
    {
        const auto keys = factors[i]->keys();
        for (std::size_t a = 0; a < keys.size(); ++a)
        {
            const auto [o, n] = layout.at.at(keys[a]);
            J.block(row, o, lins[i].residual.size(), n) += lins[i].jacobians[a];
        }
        row += lins[i].residual.size();
    }

    // Unit-norm columns: the scaling the LDLT path applies to the Hessian.
    const Eigen::VectorXd norms = J.colwise().norm();
    if (!(norms.minCoeff() > 0.0)) return out;  // a variable nothing touches
    const Eigen::VectorXd scale = norms.cwiseInverse();
    J = J * scale.asDiagonal();

    const Eigen::HouseholderQR<Eigen::MatrixXd> qr(J);
    if (!(qr.matrixQR().diagonal().cwiseAbs().minCoeff() > kQrRankTolerance)) return out;
    const auto R = qr.matrixQR().topRows(layout.size).triangularView<Eigen::Upper>();

    // J S = Q R, so (S H S)^-1 = R^-1 R^-T and the block of H^-1 is Z^T Z
    // with Z = R^-T S E: one triangular solve.
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        Eigen::Index n = 0;
        bool known = true;
        for (Key k : groups[g])
        {
            const auto it = layout.at.find(k);
            if (it == layout.at.end())
            {
                known = false;
                break;
            }
            n += it->second.second;
        }
        if (!known || n == 0) continue;
        Eigen::MatrixXd z = Eigen::MatrixXd::Zero(layout.size, n);
        Eigen::Index col = 0;
        for (Key k : groups[g])
        {
            const auto [o, dim] = layout.at.at(k);
            for (Eigen::Index i = 0; i < dim; ++i) z(o + i, col + i) = scale[o + i];
            col += dim;
        }
        R.transpose().solveInPlace(z);
        const Eigen::MatrixXd cov = z.transpose() * z;
        if (cov.allFinite()) out[g] = 0.5 * (cov + cov.transpose());
    }
    return out;
}

}  // namespace

SolverCache::SolverCache() : impl_(std::make_unique<Impl>()) {}
SolverCache::~SolverCache() = default;
SolverCache::SolverCache(const SolverCache&) : impl_(std::make_unique<Impl>()) {}
SolverCache& SolverCache::operator=(const SolverCache& other)
{
    if (this != &other) impl_ = std::make_unique<Impl>();
    return *this;
}
SolverCache::SolverCache(SolverCache&&) noexcept = default;
SolverCache& SolverCache::operator=(SolverCache&&) noexcept = default;
std::uint64_t SolverCache::analyses() const { return impl_ ? impl_->analyses : 0; }
std::uint64_t SolverCache::reuses() const { return impl_ ? impl_->reuses : 0; }
std::uint64_t SolverCache::covariancesFromSolve() const { return impl_ ? impl_->covariances_from_solve : 0; }

double totalCost(const FactorList& factors, const Values& values)
{
    double cost = 0.0;
    for (const auto& f : factors) cost += f->error(values);
    return cost;
}

namespace
{

OptimizeReport solve(const FactorList& factors, Values& values, const LmParams& params, SolverCache::Impl& solver)
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto out_of_time = [&] {
        return params.time_budget && Clock::now() - start > *params.time_budget;
    };

    OptimizeReport report;
    const Layout layout = layoutOf(values);
    if (layout.size == 0)
    {
        report.converged = true;
        report.stop_reason = "nothing to solve";
        return report;
    }

    double lambda = params.initial_lambda;
    double nu = 2.0;
    solver.last_usable = false;
    const auto& ldlt = solver.ldlt;

    NormalEquations ne = assemble(factors, values, layout);
    report.initial_cost = ne.cost;
    report.final_cost = ne.cost;

    for (report.iterations = 0; report.iterations < params.max_iterations;)
    {
        if (out_of_time())
        {
            report.stop_reason = "time budget";
            return report;
        }

        // Solved in Jacobi-scaled variables: the same step, better
        // conditioned, and Marquardt's lambda * diag(H) damping becomes
        // lambda * I. The factorisation is then the one the covariance wants.
        const Eigen::VectorXd scale = jacobiScale(ne.H, params.min_diagonal);
        Eigen::SparseMatrix<double> damped = scaled(ne.H, scale);
        for (Eigen::Index i = 0; i < layout.size; ++i) damped.coeffRef(i, i) += lambda;
        // The damping only adds to a diagonal every variable already has, so
        // this is H's pattern, and SolverCache reuses the analysis while it
        // holds.
        damped.makeCompressed();
        solver.factorize(damped);
        Eigen::VectorXd delta;
        if (ldlt.info() == Eigen::Success) delta = scale.cwiseProduct(ldlt.solve(-scale.cwiseProduct(ne.g)));
        const bool solved = ldlt.info() == Eigen::Success && delta.allFinite();

        // Gain ratio: actual decrease over the decrease the quadratic model predicted.
        const Eigen::VectorXd Hd = ne.H.selfadjointView<Eigen::Lower>() * (solved ? delta : Eigen::VectorXd::Zero(layout.size));
        const double predicted = solved ? -(ne.g.dot(delta) + 0.5 * delta.dot(Hd)) : 0.0;

        // A step already inside the tolerance is taken and the solve ends,
        // without asking the cost: in ECEF the cost cannot resolve it. A
        // position of 6e6 m is known to ~1e-9 m, which an IMU factor's
        // whitening turns into ~1e-4 per residual, so a step predicting 1e-7
        // routinely "fails" to lower the cost, and was rejected, damped and
        // retried until the iteration cap -- on every keyframe.
        if (solved && std::sqrt(std::max(0.0, delta.dot(Hd))) <= params.step_tolerance)
        {
            values = stepped(values, layout, delta);
            ++report.iterations;
            report.final_cost = ne.cost - predicted;
            report.converged = true;
            report.stop_reason = "converged";
            // Damping small enough not to mask a zero pivot: this
            // factorisation serves the covariance too.
            solver.last_usable = lambda <= 1e-2 * kPivotTolerance;
            solver.last_scale = scale;
            solver.last_layout = layout.at;
            return report;
        }

        double new_cost = 0.0;
        Values candidate;
        if (solved)
        {
            candidate = stepped(values, layout, delta);
            new_cost = totalCost(factors, candidate);
        }
        // Near the minimum the cost stops resolving the step: an error e moves
        // it by ~e^2, which falls below the cost's own rounding long before e
        // is small. A step the model says is that small is accepted on the
        // model's word rather than rejected for failing to lower a number that
        // can no longer move -- otherwise the solve stalls ~1e-9 short.
        const double rounding = 64.0 * std::numeric_limits<double>::epsilon() * ne.cost;
        const bool within_rounding = predicted <= rounding && new_cost <= ne.cost + rounding;
        if (!solved || !std::isfinite(new_cost) || (new_cost >= ne.cost && !within_rounding))
        {
            ++report.rejected_steps;
            lambda *= nu;
            nu *= 2.0;
            if (lambda > params.max_lambda)
            {
                // No step lowers the cost at any damping: a minimum to within
                // what the arithmetic resolves.
                report.converged = true;
                report.stop_reason = "no descent step";
                return report;
            }
            continue;
        }

        ++report.iterations;
        const double decrease = std::max(0.0, ne.cost - new_cost);
        const double rho = predicted > 0.0 ? decrease / predicted : 0.0;
        lambda *= std::max(1.0 / 3.0, 1.0 - std::pow(2.0 * rho - 1.0, 3));
        nu = 2.0;
        values = std::move(candidate);

        const double step = std::sqrt(std::max(0.0, delta.dot(Hd)));
        const bool small = step <= params.step_tolerance || decrease <= params.relative_tolerance * ne.cost;
        ne = assemble(factors, values, layout);
        report.final_cost = ne.cost;
        if (small)
        {
            report.converged = true;
            report.stop_reason = "converged";
            return report;
        }
    }
    report.stop_reason = "iteration limit";
    return report;
}

}  // namespace

OptimizeReport optimize(const FactorList& factors, Values& values, const LmParams& params, SolverCache* cache,
                        std::span<const Key> covariance_keys)
{
    SolverCache local;
    SolverCache::Impl& solver = (cache ? *cache : local).impl();
    OptimizeReport report = solve(factors, values, params, solver);
    if (covariance_keys.empty()) return report;
    if (solver.last_usable && pivotsResolved(solver.ldlt))
    {
        report.covariance = blockCovariance(solver.ldlt, solver.last_scale, solver.last_layout, covariance_keys);
        ++solver.covariances_from_solve;
    }
    else
    {
        report.covariance = jointCovariance(factors, values, covariance_keys, cache ? cache : &local);
    }
    return report;
}

std::vector<std::optional<Eigen::MatrixXd>> jointCovariances(const FactorList& factors, const Values& values,
                                                             const std::vector<std::vector<Key>>& groups,
                                                             SolverCache* cache)
{
    std::vector<std::optional<Eigen::MatrixXd>> out(groups.size());
    const Layout layout = layoutOf(values);
    if (layout.size == 0) return out;
    SolverCache local;
    SolverCache::Impl& solver = (cache ? *cache : local).impl();
    const NormalEquations ne = assemble(factors, values, layout);
    solver.last_usable = false;  // the factorisation is about to be this one

    const Eigen::VectorXd scale = jacobiScale(ne.H, std::numeric_limits<double>::min());
    Eigen::SparseMatrix<double> Hs = scaled(ne.H, scale);
    Hs.makeCompressed();
    solver.factorize(Hs);
    if (!pivotsResolved(solver.ldlt)) return qrCovariances(factors, values, layout, groups);
    for (std::size_t g = 0; g < groups.size(); ++g) out[g] = blockCovariance(solver.ldlt, scale, layout.at, groups[g]);
    return out;
}

std::optional<Eigen::MatrixXd> jointCovariance(const FactorList& factors, const Values& values,
                                               std::span<const Key> keys, SolverCache* cache)
{
    return jointCovariances(factors, values, {std::vector<Key>(keys.begin(), keys.end())}, cache).front();
}

}  // namespace factor_graph
