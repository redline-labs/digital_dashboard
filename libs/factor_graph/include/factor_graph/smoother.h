#pragma once

// The two smoothers, one set of factors.
//
// FixedLagSmoother keeps the variables stamped within `lag` seconds of the
// newest, solves over them on every update, and folds everything older into a
// LinearPrior. With a lag of zero it is a (iterated) Kalman filter; with an
// infinite lag it never forgets. BatchSmoother holds every variable of a
// drive and solves once -- for linear-Gaussian models exactly the RTS
// smoother, and the nonlinear generalisation of it otherwise.
//
// A variable stamped kStatic (a lever arm, a mounting angle) is never
// marginalised: it stays in the window for the smoother's life and the
// information about it accumulates in the marginal priors.

#include "factor_graph/linear_prior.h"
#include "factor_graph/optimizer.h"

#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace factor_graph
{

inline constexpr double kStatic = std::numeric_limits<double>::infinity();

struct UpdateReport
{
    bool ok = false;
    std::string error;  // why the update was refused; the state is unchanged
    OptimizeReport optimize;
    std::size_t marginalized = 0;
    // The joint covariance of the keys update() was asked for, when it could
    // be computed.
    std::optional<Eigen::MatrixXd> covariance;
};

struct FixedLagParams
{
    double lag = 1.0;  // seconds, in whatever time base the stamps use
    LmParams lm;
    double rank_tolerance = 1e-12;
};

class FixedLagSmoother
{
  public:
    explicit FixedLagSmoother(FixedLagParams params = {});

    // Adds variables (each needs a stamp, kStatic for one that is never
    // marginalised) and factors, solves, and marginalises what fell out of
    // the window. Refuses the whole update -- leaving the state as it was --
    // if any factor or value is malformed.
    //
    // `covariance_keys`, if any, get their joint covariance in the report,
    // computed between the solve and the marginalisation: marginalising by
    // Schur complement leaves the others' marginal unchanged, and before it
    // the window still has the solve's sparsity pattern, so SolverCache
    // reuses the solve's symbolic analysis instead of making a new one.
    UpdateReport update(const FactorList& factors, const Values& new_values, const std::map<Key, double>& stamps,
                        std::span<const Key> covariance_keys = {});

    const Values& estimate() const { return values_; }
    const FactorList& factors() const { return factors_; }
    std::optional<double> newestStamp() const;
    std::optional<double> stamp(Key key) const;

    std::optional<Eigen::MatrixXd> covariance(Key key) const;
    std::optional<Eigen::MatrixXd> jointCovariance(std::span<const Key> keys) const;
    const SolverCache& solverCache() const { return cache_; }

  private:
    std::size_t marginalizeBefore(double cutoff);

    FixedLagParams params_;
    Values values_;
    FactorList factors_;
    std::map<Key, double> stamps_;
    mutable SolverCache cache_;
};

class BatchSmoother
{
  public:
    explicit BatchSmoother(LmParams params = {});

    // Same checks as FixedLagSmoother::update, but nothing is solved yet.
    UpdateReport add(const FactorList& factors, const Values& new_values);

    OptimizeReport optimize();

    const Values& estimate() const { return values_; }
    const FactorList& factors() const { return factors_; }
    std::optional<Eigen::MatrixXd> covariance(Key key) const;

  private:
    LmParams params_;
    Values values_;
    FactorList factors_;
};

// The checks both smoothers run on an incoming batch, exposed for tests:
// an error string, or empty.
std::string updateProblem(const FactorList& factors, const Values& existing, const Values& new_values);

}  // namespace factor_graph
