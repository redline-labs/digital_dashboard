#pragma once

// What is left of a set of factors after their variables are marginalised:
// a quadratic in the tangent spaces of the variables they touched, frozen at
// the point where it was computed.
//
//     cost(x) = 0.5 * |R d + e|^2,   d = localCoordinates(x_lin, x) stacked
//
// with R^T R = H and R^T e = g from the Schur complement. The linearisation
// point x_lin never moves again. That is the whole of "first-estimate
// Jacobians" as this library applies it: re-linearising a marginal prior at
// a later estimate would invent information the discarded factors never
// carried, and the smoother would become confidently wrong about exactly the
// directions it cannot observe (yaw, before the car has turned).

#include "factor_graph/factor.h"

#include <Eigen/Core>

#include <memory>
#include <vector>

namespace factor_graph
{

class LinearPrior final : public Factor
{
  public:
    // H must be symmetric positive semi-definite over the stacked tangent
    // spaces of `keys` in order. Eigen-directions with eigenvalue below
    // rank_tolerance * the largest carry no information and are dropped, so
    // dim() is the rank. Returns null when nothing is left.
    static std::shared_ptr<LinearPrior> fromHessian(const std::vector<Key>& keys, const Values& linearization_point,
                                                    const Eigen::MatrixXd& H, const Eigen::VectorXd& g,
                                                    double rank_tolerance = 1e-12);

    std::size_t dim() const override { return static_cast<std::size_t>(r_.rows()); }
    Linearization linearize(const Values& values) const override;
    Eigen::VectorXd residual(const Values& values) const override;
    std::string name() const override { return "marginal prior"; }

    const Values& linearizationPoint() const { return lin_; }
    const Eigen::MatrixXd& sqrtInformation() const { return r_; }

  private:
    LinearPrior(const std::vector<Key>& keys, Values lin, Eigen::MatrixXd r, Eigen::VectorXd e);

    Eigen::VectorXd delta(const Values& values) const;

    Values lin_;
    Eigen::MatrixXd r_;
    Eigen::VectorXd e_;
};

}  // namespace factor_graph
