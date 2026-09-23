#pragma once

// A factor: a residual over a few variables whose squared norm is (twice) its
// cost. Everything the optimiser knows about a sensor is what its factors say
// here -- the residual and its Jacobian with respect to each variable's
// tangent space, already whitened, so that 0.5 * |r|^2 is the negative log
// likelihood. Robust losses live inside the residual too (csym's noise
// models), so the Jacobians include them exactly.

#include "factor_graph/key.h"
#include "factor_graph/values.h"

#include <Eigen/Core>

#include <span>
#include <string>
#include <vector>

namespace factor_graph
{

struct Linearization
{
    Eigen::VectorXd residual;
    // One block per key, residual dim x that variable's tangent dim.
    std::vector<Eigen::MatrixXd> jacobians;
};

class Factor
{
  public:
    virtual ~Factor() = default;

    std::span<const Key> keys() const { return keys_; }

    virtual std::size_t dim() const = 0;

    // Residual and Jacobians at `values`, which must hold every key.
    virtual Linearization linearize(const Values& values) const = 0;

    // Residual only: cheaper, and what a step-acceptance test needs.
    virtual Eigen::VectorXd residual(const Values& values) const = 0;

    // Short description for logs and error messages.
    virtual std::string name() const = 0;

    double error(const Values& values) const { return 0.5 * residual(values).squaredNorm(); }

  protected:
    explicit Factor(std::vector<Key> keys) : keys_(std::move(keys)) {}

  private:
    std::vector<Key> keys_;
};

// What is wrong with a factor as seen from `values`, or empty when it is
// usable: every key present, distinct, Jacobians shaped for their variables,
// residual and Jacobians finite. Run on every factor before it can touch the
// optimiser, so a bad sensor sample is refused at the door rather than
// turning the whole window to NaN on the next solve.
std::string factorProblem(const Factor& factor, const Values& values);

}  // namespace factor_graph
