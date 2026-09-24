#include "factor_graph/smoother.h"

#include <Eigen/Cholesky>
#include <Eigen/Eigenvalues>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace factor_graph
{

namespace
{

// Merge new values into a copy of the existing ones, refusing duplicates.
std::string merge(Values& into, const Values& new_values)
{
    for (Key k : new_values.keys())
    {
        if (into.contains(k)) return fmt::format("variable {} already exists", keyName(k));
        if (!new_values.variable(k).isFinite()) return fmt::format("variable {} is not finite", keyName(k));
        into.insert(k, new_values.variable(k).clone());
    }
    return {};
}

Eigen::MatrixXd pseudoInverse(const Eigen::MatrixXd& a, double tolerance)
{
    // Usually invertible: Cholesky, on the Jacobi-scaled matrix so the
    // condition estimate is about the problem and not its units, when that
    // estimate is clear of the tolerance by a wide margin.
    {
        const Eigen::VectorXd s = a.diagonal().cwiseMax(std::numeric_limits<double>::min()).cwiseSqrt().cwiseInverse();
        const Eigen::MatrixXd as = s.asDiagonal() * (0.5 * (a + a.transpose())) * s.asDiagonal();
        const Eigen::LLT<Eigen::MatrixXd> llt(as);
        if (llt.info() == Eigen::Success && llt.rcond() > 1e4 * tolerance)
            return s.asDiagonal() * llt.solve(Eigen::MatrixXd::Identity(a.rows(), a.cols())) * s.asDiagonal();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(0.5 * (a + a.transpose()));
    const Eigen::VectorXd& l = eig.eigenvalues();
    const double largest = l.size() ? l.cwiseAbs().maxCoeff() : 0.0;
    Eigen::VectorXd inv(l.size());
    for (Eigen::Index i = 0; i < l.size(); ++i) inv[i] = l[i] > tolerance * largest ? 1.0 / l[i] : 0.0;
    return eig.eigenvectors() * inv.asDiagonal() * eig.eigenvectors().transpose();
}

}  // namespace

std::string updateProblem(const FactorList& factors, const Values& existing, const Values& new_values)
{
    Values merged = existing;
    if (auto e = merge(merged, new_values); !e.empty()) return e;
    for (const auto& f : factors)
    {
        if (!f) return "null factor";
        if (auto e = factorProblem(*f, merged); !e.empty()) return e;
    }
    return {};
}

// ---- FixedLagSmoother ----------------------------------------------------

FixedLagSmoother::FixedLagSmoother(FixedLagParams params) : params_(std::move(params)) {}

std::optional<double> FixedLagSmoother::newestStamp() const
{
    std::optional<double> newest;
    for (const auto& [k, t] : stamps_)
        if (t != kStatic && (!newest || t > *newest)) newest = t;
    return newest;
}

std::optional<double> FixedLagSmoother::stamp(Key key) const
{
    auto it = stamps_.find(key);
    if (it == stamps_.end()) return std::nullopt;
    return it->second;
}

UpdateReport FixedLagSmoother::update(const FactorList& factors, const Values& new_values,
                                      const std::map<Key, double>& stamps, std::span<const Key> covariance_keys)
{
    UpdateReport report;
    for (Key k : new_values.keys())
    {
        auto it = stamps.find(k);
        if (it == stamps.end())
        {
            report.error = fmt::format("variable {} has no stamp", keyName(k));
            return report;
        }
        if (std::isnan(it->second) || it->second == -kStatic)
        {
            report.error = fmt::format("variable {} has an invalid stamp", keyName(k));
            return report;
        }
    }
    for (const auto& [k, t] : stamps)
    {
        if (!new_values.contains(k))
        {
            report.error = fmt::format("stamp for {}, which is not a new variable", keyName(k));
            return report;
        }
    }

    Values merged = values_;
    if (auto e = merge(merged, new_values); !e.empty())
    {
        report.error = e;
        return report;
    }
    for (const auto& f : factors)
    {
        if (!f)
        {
            report.error = "null factor";
            return report;
        }
        if (auto e = factorProblem(*f, merged); !e.empty())
        {
            report.error = e;
            return report;
        }
    }

    values_ = std::move(merged);
    for (const auto& [k, t] : stamps) stamps_[k] = t;
    factors_.insert(factors_.end(), factors.begin(), factors.end());

    report.optimize = optimize(factors_, values_, params_.lm, &cache_, covariance_keys);
    report.covariance = std::move(report.optimize.covariance);
    if (const auto newest = newestStamp()) report.marginalized = marginalizeBefore(*newest - params_.lag);
    report.ok = true;
    return report;
}

std::size_t FixedLagSmoother::marginalizeBefore(double cutoff)
{
    std::vector<Key> marg;
    for (const auto& [k, t] : stamps_)
        if (t < cutoff) marg.push_back(k);
    if (marg.empty()) return 0;
    const std::set<Key> in_marg(marg.begin(), marg.end());

    FactorList touching, keep;
    for (const auto& f : factors_)
    {
        const auto keys = f->keys();
        const bool touches = std::any_of(keys.begin(), keys.end(), [&](Key k) { return in_marg.contains(k); });
        (touches ? touching : keep).push_back(f);
    }

    // The Markov blanket: everything the discarded factors also touched.
    std::vector<Key> blanket;
    std::set<Key> in_blanket;
    for (const auto& f : touching)
        for (Key k : f->keys())
            if (!in_marg.contains(k) && in_blanket.insert(k).second) blanket.push_back(k);

    std::map<Key, std::pair<Eigen::Index, Eigen::Index>> at;
    Eigen::Index n = 0;
    for (const auto* list : {&marg, &blanket})
        for (Key k : *list)
        {
            const auto d = static_cast<Eigen::Index>(values_.variable(k).tangentDim());
            at.emplace(k, std::make_pair(n, d));
            n += d;
        }
    Eigen::Index nm = 0;
    for (Key k : marg) nm += at.at(k).second;

    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd g = Eigen::VectorXd::Zero(n);
    for (const auto& f : touching)
    {
        const Linearization lin = f->linearize(values_);
        const auto keys = f->keys();
        for (std::size_t a = 0; a < keys.size(); ++a)
        {
            const auto [oa, na] = at.at(keys[a]);
            g.segment(oa, na) += lin.jacobians[a].transpose() * lin.residual;
            for (std::size_t b = 0; b < keys.size(); ++b)
            {
                const auto [ob, nb] = at.at(keys[b]);
                H.block(oa, ob, na, nb) += lin.jacobians[a].transpose() * lin.jacobians[b];
            }
        }
    }

    if (!blanket.empty())
    {
        const Eigen::Index nb = n - nm;
        const Eigen::MatrixXd hmm_inv = pseudoInverse(H.topLeftCorner(nm, nm), params_.rank_tolerance);
        const Eigen::MatrixXd hbm = H.bottomLeftCorner(nb, nm);
        const Eigen::MatrixXd hs = H.bottomRightCorner(nb, nb) - hbm * hmm_inv * hbm.transpose();
        const Eigen::VectorXd gs = g.tail(nb) - hbm * hmm_inv * g.head(nm);
        if (auto prior = LinearPrior::fromHessian(blanket, values_, hs, gs, params_.rank_tolerance))
            keep.push_back(std::move(prior));
    }

    factors_ = std::move(keep);
    for (Key k : marg)
    {
        values_.erase(k);
        stamps_.erase(k);
    }
    return marg.size();
}

std::optional<Eigen::MatrixXd> FixedLagSmoother::covariance(Key key) const
{
    return jointCovariance(std::span<const Key>(&key, 1));
}

std::optional<Eigen::MatrixXd> FixedLagSmoother::jointCovariance(std::span<const Key> keys) const
{
    for (Key k : keys)
        if (!values_.contains(k)) return std::nullopt;
    return factor_graph::jointCovariance(factors_, values_, keys, &cache_);
}

// ---- BatchSmoother -------------------------------------------------------

BatchSmoother::BatchSmoother(LmParams params) : params_(std::move(params)) {}

UpdateReport BatchSmoother::add(const FactorList& factors, const Values& new_values)
{
    UpdateReport report;
    if (auto e = updateProblem(factors, values_, new_values); !e.empty())
    {
        report.error = e;
        return report;
    }
    merge(values_, new_values);
    factors_.insert(factors_.end(), factors.begin(), factors.end());
    report.ok = true;
    return report;
}

OptimizeReport BatchSmoother::optimize()
{
    return factor_graph::optimize(factors_, values_, params_);
}

std::optional<Eigen::MatrixXd> BatchSmoother::covariance(Key key) const
{
    if (!values_.contains(key)) return std::nullopt;
    return jointCovariance(factors_, values_, std::span<const Key>(&key, 1));
}

}  // namespace factor_graph
