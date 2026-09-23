#include "factor_graph/linear_prior.h"

#include <Eigen/Eigenvalues>

#include <stdexcept>

namespace factor_graph
{

std::shared_ptr<LinearPrior> LinearPrior::fromHessian(const std::vector<Key>& keys, const Values& linearization_point,
                                                      const Eigen::MatrixXd& H, const Eigen::VectorXd& g,
                                                      double rank_tolerance)
{
    Values lin;
    Eigen::Index n = 0;
    for (Key k : keys)
    {
        lin.insert(k, linearization_point.variable(k).clone());
        n += static_cast<Eigen::Index>(linearization_point.variable(k).tangentDim());
    }
    if (H.rows() != n || H.cols() != n || g.size() != n)
        throw std::invalid_argument("LinearPrior: Hessian does not match the variables' tangent dimensions");

    // H = V diag(l) V^T. R = diag(sqrt l) V^T and e = diag(1/sqrt l) V^T g
    // over the directions that carry information: R^T R = H and R^T e = g on
    // that subspace, and nothing is claimed about the rest.
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(0.5 * (H + H.transpose()));
    if (eig.info() != Eigen::Success) throw std::runtime_error("LinearPrior: eigendecomposition failed");
    const Eigen::VectorXd& l = eig.eigenvalues();
    const double largest = l.size() ? l.maxCoeff() : 0.0;
    if (!(largest > 0.0)) return nullptr;

    std::vector<Eigen::Index> kept;
    for (Eigen::Index i = 0; i < l.size(); ++i)
        if (l[i] > rank_tolerance * largest) kept.push_back(i);
    if (kept.empty()) return nullptr;

    const auto rank = static_cast<Eigen::Index>(kept.size());
    Eigen::MatrixXd r(rank, n);
    Eigen::VectorXd e(rank);
    for (Eigen::Index row = 0; row < rank; ++row)
    {
        const Eigen::Index i = kept[static_cast<std::size_t>(row)];
        const double s = std::sqrt(l[i]);
        const auto v = eig.eigenvectors().col(i);
        r.row(row) = s * v.transpose();
        e[row] = v.dot(g) / s;
    }
    return std::shared_ptr<LinearPrior>(new LinearPrior(keys, std::move(lin), std::move(r), std::move(e)));
}

LinearPrior::LinearPrior(const std::vector<Key>& keys, Values lin, Eigen::MatrixXd r, Eigen::VectorXd e)
    : Factor(keys), lin_(std::move(lin)), r_(std::move(r)), e_(std::move(e))
{
}

Eigen::VectorXd LinearPrior::delta(const Values& values) const
{
    Eigen::VectorXd d(r_.cols());
    Eigen::Index at = 0;
    for (Key k : keys())
    {
        const Eigen::VectorXd xi = lin_.variable(k).localCoordinates(values.variable(k));
        d.segment(at, xi.size()) = xi;
        at += xi.size();
    }
    return d;
}

Eigen::VectorXd LinearPrior::residual(const Values& values) const
{
    return r_ * delta(values) + e_;
}

Linearization LinearPrior::linearize(const Values& values) const
{
    Linearization out;
    out.residual = residual(values);
    Eigen::Index at = 0;
    for (Key k : keys())
    {
        const Variable& lin = lin_.variable(k);
        const auto n = static_cast<Eigen::Index>(lin.tangentDim());
        out.jacobians.push_back(r_.middleCols(at, n) * lin.localCoordinatesJacobian(values.variable(k)));
        at += n;
    }
    return out;
}

}  // namespace factor_graph
