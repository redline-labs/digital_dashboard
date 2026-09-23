#pragma once

// Levenberg-Marquardt over a set of factors, and the covariance of the
// result. Both smoothers are this plus bookkeeping.
//
// The normal equations are assembled block-sparse and solved with Eigen's
// simplicial LDLT under an AMD ordering: a fixed-lag window is block-banded
// plus an arrow for the static variables, and a whole-drive batch is the same
// shape much longer, so the fill stays linear in the number of keyframes.

#include "factor_graph/factor.h"
#include "factor_graph/values.h"

#include <Eigen/Core>

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace factor_graph
{

using FactorList = std::vector<std::shared_ptr<const Factor>>;

struct LmParams
{
    int max_iterations = 20;
    // Converged when an accepted step is this small in the whitened metric,
    // sqrt(d^T H d): a step of 1e-8 sigma. A test on the cost decrease alone
    // stops far earlier than it looks -- the cost is dominated by the
    // residual that cannot be removed, and a parameter error e only moves it
    // by e^2 -- so a relative cost tolerance of 1e-9 still leaves errors of
    // ~1e-4 sigma.
    double step_tolerance = 1e-8;
    // Optional early exit on the cost for a caller with a time budget: stop
    // when an accepted step lowers the cost by less than this fraction.
    double relative_tolerance = 0.0;
    double initial_lambda = 1e-6;
    double max_lambda = 1e12;
    // Damping is lambda * max(diag(H), this): a variable no factor constrains
    // still gets a finite, zero step instead of a singular solve.
    double min_diagonal = 1e-9;
    // Wall-clock budget; the best estimate so far is kept when it runs out.
    std::optional<std::chrono::duration<double>> time_budget;
};

struct OptimizeReport
{
    int iterations = 0;
    int rejected_steps = 0;
    double initial_cost = 0.0;
    double final_cost = 0.0;
    bool converged = false;
    std::string stop_reason;
};

OptimizeReport optimize(const FactorList& factors, Values& values, const LmParams& params = {});

// Total cost 0.5 * sum |r|^2.
double totalCost(const FactorList& factors, const Values& values);

// Joint covariance of `keys` (stacked tangent spaces, in order) at `values`,
// from the inverse of the Gauss-Newton Hessian. Empty when the Hessian is
// singular -- some direction no factor constrains -- because the honest
// answer then is "unknown", and an inverse of a near-singular matrix is a
// plausible-looking wrong one.
std::optional<Eigen::MatrixXd> jointCovariance(const FactorList& factors, const Values& values,
                                               std::span<const Key> keys);

// The joint covariance of each group of keys, from one factorisation. A
// whole drive has thousands of keyframes; factorising once per keyframe is
// quadratic in the drive's length, this is linear. Empty entries (or all of
// them, for a singular Hessian) as jointCovariance.
std::vector<std::optional<Eigen::MatrixXd>> jointCovariances(const FactorList& factors, const Values& values,
                                                             const std::vector<std::vector<Key>>& groups);

}  // namespace factor_graph
