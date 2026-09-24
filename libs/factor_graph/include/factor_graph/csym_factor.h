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
//
// A large constant sqrt-information matrix can instead be applied after the
// program runs -- construct with `Whitened{}` and pass it first. That is
// exact for a constant matrix, and it keeps a dense N x N product out of the
// traced residual, where it would be differentiated symbolically: the IMU
// factor's 9 x 9 took most of its 54 s build. Only for a residual whose
// robust loss (if any) is not applied to the whitened value inside F.

#include "factor_graph/factor.h"

#include "csym/function.h"

#include <array>
#include <stdexcept>
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

struct Whitened
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

    CsymFactor(Whitened, std::string name, const std::array<Key, kNumVars>& keys, Eigen::MatrixXd sqrt_info,
               const P&... params)
        : Factor(std::vector<Key>(keys.begin(), keys.end())), name_(std::move(name)), params_(params...),
          whiten_(std::move(sqrt_info))
    {
        if (whiten_.rows() != static_cast<Eigen::Index>(kDim) || whiten_.cols() != static_cast<Eigen::Index>(kDim))
            throw std::invalid_argument(name_ + ": sqrt-information must be square in the residual's dimension");
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
        if (whiten_.size() != 0)
        {
            out.residual = whiten_ * out.residual;
            for (auto& j : out.jacobians) j = whiten_ * j;
        }
        return out;
    }

    template <std::size_t... I, std::size_t... J>
    Eigen::VectorXd residualImpl(const Values& values, std::index_sequence<I...>, std::index_sequence<J...>) const
    {
        const auto k = keys();
        Eigen::VectorXd r = toEigen(Fn::eval(values.at<VarType<I>>(k[I])..., std::get<J>(params_)...));
        if (whiten_.size() != 0) r = whiten_ * r;
        return r;
    }

    template <std::size_t R, std::size_t C>
    static Eigen::MatrixXd toEigen(const csym::Matrix<double, R, C>& m)
    {
        return Eigen::Map<const Eigen::MatrixXd>(m.data.data(), static_cast<Eigen::Index>(R),
                                                 static_cast<Eigen::Index>(C));
    }

    std::string name_;
    std::tuple<P...> params_;
    Eigen::MatrixXd whiten_;  // empty: F whitens its own residual
};

}  // namespace factor_graph
