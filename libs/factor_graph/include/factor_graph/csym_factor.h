#pragma once

// A factor whose residual is a csym function: written once as a generic
// lambda, differentiated at compile time. The first arguments are the
// variables (looked up by key), the rest are constants fixed when the factor
// is built -- a measurement, a sqrt-information matrix, a lever arm.
//
//     constexpr auto kPrior = [](auto x, auto mean, auto sqrt_info) {
//         return sqrt_info * (x - mean);
//     };
//     using PriorFactor = CsymFactor<kPrior, Vars<Vector3>, Params<Vector3, Matrix33>>;
//     auto f = std::make_shared<PriorFactor>("prior", {key}, mean, sqrt_info);
//
// Each distinct instantiation compiles a residual program; a heavy one costs
// seconds of compile time, which is why the estimator builds each of its
// factor types in a translation unit of its own.

#include "factor_graph/factor.h"

#include "csym/function.h"

#include <array>
#include <string>
#include <tuple>
#include <utility>

namespace factor_graph
{

template <class... V>
struct Vars
{
};
template <class... P>
struct Params
{
};

template <auto F, class VarList, class ParamList>
class CsymFactor;

template <auto F, class... V, class... P>
class CsymFactor<F, Vars<V...>, Params<P...>> final : public Factor
{
  public:
    using Fn = csym::Function<F, V..., P...>;
    static constexpr std::size_t kNumVars = sizeof...(V);
    static constexpr std::size_t kDim = Fn::output_dim;

    static_assert(kNumVars > 0, "a factor needs at least one variable");
    static_assert(Fn::output_dim == Fn::output_tangent_dim,
                  "a factor's residual must be a vector, not a Lie group value");

    CsymFactor(std::string name, const std::array<Key, kNumVars>& keys, const P&... params)
        : Factor(std::vector<Key>(keys.begin(), keys.end())), name_(std::move(name)), params_(params...)
    {
    }

    std::size_t dim() const override { return kDim; }
    std::string name() const override { return name_; }

    Linearization linearize(const Values& values) const override
    {
        return linearizeImpl(values, std::index_sequence_for<V...>{}, std::index_sequence_for<P...>{});
    }

    Eigen::VectorXd residual(const Values& values) const override
    {
        return residualImpl(values, std::index_sequence_for<V...>{}, std::index_sequence_for<P...>{});
    }

    const std::tuple<P...>& params() const { return params_; }

  private:
    template <std::size_t I>
    using VarType = std::tuple_element_t<I, std::tuple<V...>>;

    template <std::size_t... I, std::size_t... J>
    Linearization linearizeImpl(const Values& values, std::index_sequence<I...>, std::index_sequence<J...>) const
    {
        const auto k = keys();
        std::tuple<typename Fn::template Block<I>...> blocks;
        const auto value = Fn::template evaluate<I...>(values.at<VarType<I>>(k[I])..., std::get<J>(params_)...,
                                                       &std::get<I>(blocks)...);
        Linearization out;
        out.residual = toEigen(value);
        out.jacobians.reserve(kNumVars);
        (out.jacobians.push_back(toEigen(std::get<I>(blocks))), ...);
        return out;
    }

    template <std::size_t... I, std::size_t... J>
    Eigen::VectorXd residualImpl(const Values& values, std::index_sequence<I...>, std::index_sequence<J...>) const
    {
        const auto k = keys();
        return toEigen(Fn::eval(values.at<VarType<I>>(k[I])..., std::get<J>(params_)...));
    }

    template <std::size_t R, std::size_t C>
    static Eigen::MatrixXd toEigen(const csym::Matrix<double, R, C>& m)
    {
        return Eigen::Map<const Eigen::MatrixXd>(m.data.data(), static_cast<Eigen::Index>(R),
                                                 static_cast<Eigen::Index>(C));
    }

    std::string name_;
    std::tuple<P...> params_;
};

}  // namespace factor_graph
