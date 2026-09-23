#include "factor_graph/factor.h"

#include <spdlog/fmt/fmt.h>

#include <set>

namespace factor_graph
{

std::string factorProblem(const Factor& factor, const Values& values)
{
    const auto keys = factor.keys();
    if (keys.empty()) return fmt::format("{}: no variables", factor.name());
    if (factor.dim() == 0) return fmt::format("{}: empty residual", factor.name());

    std::set<Key> seen;
    for (Key k : keys)
    {
        if (!seen.insert(k).second) return fmt::format("{}: variable {} appears twice", factor.name(), keyName(k));
        if (!values.contains(k)) return fmt::format("{}: unknown variable {}", factor.name(), keyName(k));
    }

    Linearization lin;
    try
    {
        lin = factor.linearize(values);
    }
    catch (const std::exception& e)
    {
        return fmt::format("{}: {}", factor.name(), e.what());
    }

    const auto dim = static_cast<Eigen::Index>(factor.dim());
    if (lin.residual.size() != dim)
        return fmt::format("{}: residual has {} rows, factor says {}", factor.name(), lin.residual.size(), dim);
    if (!lin.residual.allFinite()) return fmt::format("{}: residual is not finite", factor.name());
    if (lin.jacobians.size() != keys.size())
        return fmt::format("{}: {} Jacobian blocks for {} variables", factor.name(), lin.jacobians.size(), keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const auto& J = lin.jacobians[i];
        const auto cols = static_cast<Eigen::Index>(values.variable(keys[i]).tangentDim());
        if (J.rows() != dim || J.cols() != cols)
            return fmt::format("{}: Jacobian for {} is {}x{}, expected {}x{}", factor.name(), keyName(keys[i]),
                               J.rows(), J.cols(), dim, cols);
        if (!J.allFinite()) return fmt::format("{}: Jacobian for {} is not finite", factor.name(), keyName(keys[i]));
    }
    return {};
}

}  // namespace factor_graph
