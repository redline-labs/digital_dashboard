#include "factor_graph/optimizer.h"

#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

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
    for (const auto& f : factors)
    {
        const Linearization lin = f->linearize(values);
        ne.cost += 0.5 * lin.residual.squaredNorm();
        const auto keys = f->keys();
        for (std::size_t a = 0; a < keys.size(); ++a)
        {
            const auto [oa, na] = layout.at.at(keys[a]);
            ne.g.segment(oa, na) += lin.jacobians[a].transpose() * lin.residual;
            for (std::size_t b = 0; b < keys.size(); ++b)
            {
                const auto [ob, nb] = layout.at.at(keys[b]);
                if (ob > oa) continue;  // lower triangle: block row >= block column
                const Eigen::MatrixXd blk = lin.jacobians[a].transpose() * lin.jacobians[b];
                for (Eigen::Index r = 0; r < na; ++r)
                    for (Eigen::Index c = 0; c < nb; ++c)
                        if (oa + r >= ob + c && blk(r, c) != 0.0)
                            triplets.emplace_back(oa + r, ob + c, blk(r, c));
            }
        }
    }
    ne.H.resize(layout.size, layout.size);
    ne.H.setFromTriplets(triplets.begin(), triplets.end());
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

double totalCost(const FactorList& factors, const Values& values)
{
    double cost = 0.0;
    for (const auto& f : factors) cost += f->error(values);
    return cost;
}

OptimizeReport optimize(const FactorList& factors, Values& values, const LmParams& params)
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
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower> ldlt;

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

        const Eigen::VectorXd diag = ne.H.diagonal();
        Eigen::SparseMatrix<double> damped = ne.H;
        for (Eigen::Index i = 0; i < layout.size; ++i)
            damped.coeffRef(i, i) += lambda * std::max(diag[i], params.min_diagonal);

        // Analysed afresh every time. The assembly drops exact zeros, so the
        // pattern can change between iterations (a bias Jacobian block that
        // was zero at the first linearisation is not at the second), and a
        // factorisation reusing a stale ordering indexes past the end of it --
        // silently, with assertions off.
        ldlt.compute(damped);
        Eigen::VectorXd delta;
        if (ldlt.info() == Eigen::Success) delta = ldlt.solve(-ne.g);

        const bool solved = ldlt.info() == Eigen::Success && delta.allFinite();
        double new_cost = 0.0;
        Values candidate;
        if (solved)
        {
            candidate = stepped(values, layout, delta);
            new_cost = totalCost(factors, candidate);
        }

        // Gain ratio: actual decrease over the decrease the quadratic model predicted.
        const Eigen::VectorXd Hd = ne.H.selfadjointView<Eigen::Lower>() * (solved ? delta : Eigen::VectorXd::Zero(layout.size));
        const double predicted = solved ? -(ne.g.dot(delta) + 0.5 * delta.dot(Hd)) : 0.0;
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

std::vector<std::optional<Eigen::MatrixXd>> jointCovariances(const FactorList& factors, const Values& values,
                                                             const std::vector<std::vector<Key>>& groups)
{
    std::vector<std::optional<Eigen::MatrixXd>> out(groups.size());
    const Layout layout = layoutOf(values);
    if (layout.size == 0) return out;
    const NormalEquations ne = assemble(factors, values, layout);
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower> ldlt(ne.H);
    if (ldlt.info() != Eigen::Success) return out;

    // A zero pivot is a direction nothing constrains. The scale is relative:
    // an ECEF position and an accelerometer bias differ in information by
    // ten orders of magnitude, and both are perfectly well determined.
    const Eigen::VectorXd d = ldlt.vectorD();
    const double largest = d.cwiseAbs().maxCoeff();
    if (!(d.minCoeff() > largest * 1e-15)) return out;

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
        Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(layout.size, n);
        Eigen::Index col = 0;
        for (Key k : groups[g])
        {
            const auto [o, dim] = layout.at.at(k);
            for (Eigen::Index i = 0; i < dim; ++i) rhs(o + i, col + i) = 1.0;
            col += dim;
        }
        const Eigen::MatrixXd x = ldlt.solve(rhs);
        if (!x.allFinite()) continue;
        Eigen::MatrixXd cov(n, n);
        Eigen::Index row = 0;
        for (Key k : groups[g])
        {
            const auto [o, dim] = layout.at.at(k);
            cov.middleRows(row, dim) = x.middleRows(o, dim);
            row += dim;
        }
        out[g] = 0.5 * (cov + cov.transpose());
    }
    return out;
}

std::optional<Eigen::MatrixXd> jointCovariance(const FactorList& factors, const Values& values,
                                               std::span<const Key> keys)
{
    return jointCovariances(factors, values, {std::vector<Key>(keys.begin(), keys.end())}).front();
}

}  // namespace factor_graph
