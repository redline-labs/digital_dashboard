#pragma once

// The graph's variables: values on manifolds (a rotation, a vector, a pose),
// each moved by retracting a tangent-space step and compared by its local
// coordinates. Any type csym understands as a Lie group can be a variable --
// csym::lie<T> says how it moves, and csym differentiates local_coordinates
// for the one Jacobian the marginal priors need.

#include "factor_graph/key.h"

#include "csym/function.h"
#include "csym/lie.h"

#include <Eigen/Core>

#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace factor_graph
{

// csym's manifold operations take an epsilon that keeps them finite and
// differentiable at the identity (a rotation's log at zero angle is 0/0
// without it). One value for the whole graph, so a factor's residual and the
// retraction it is optimised through agree about where the identity is.
inline constexpr double kEpsilon = 1e-12;

namespace detail
{
inline constexpr auto kLocalCoordinates = [](auto a, auto b, auto eps) { return csym::local_coordinates(a, b, eps); };
}  // namespace detail

// A variable of unknown type: what the optimiser needs to move one.
class Variable
{
  public:
    virtual ~Variable() = default;

    virtual std::size_t tangentDim() const = 0;
    virtual std::unique_ptr<Variable> clone() const = 0;
    virtual bool isFinite() const = 0;

    // this <- this (+) delta.
    virtual void retract(std::span<const double> delta) = 0;

    // xi with other = this (+) xi. Both must be the same type.
    virtual Eigen::VectorXd localCoordinates(const Variable& other) const = 0;

    // d localCoordinates(this, other (+) e) / de at e = 0: how the local
    // coordinates of a moving variable change as the optimiser steps it.
    virtual Eigen::MatrixXd localCoordinatesJacobian(const Variable& other) const = 0;
};

template <class T>
class VariableModel final : public Variable
{
  public:
    static constexpr std::size_t kTangentDim = csym::lie<T>::tangent_dim;

    explicit VariableModel(const T& v) : value(v) {}

    T value;

    std::size_t tangentDim() const override { return kTangentDim; }
    std::unique_ptr<Variable> clone() const override { return std::make_unique<VariableModel>(value); }

    bool isFinite() const override
    {
        double s[csym::storage_dim<T>];
        csym::storage<T>::to(value, s);
        for (double x : s)
            if (!std::isfinite(x)) return false;
        return true;
    }

    void retract(std::span<const double> delta) override
    {
        if (delta.size() != kTangentDim) throw std::invalid_argument("retract: wrong tangent dimension");
        csym::Vector<double, kTangentDim> d;
        for (std::size_t i = 0; i < kTangentDim; ++i) d[i] = delta[i];
        value = csym::retract(value, d, kEpsilon);
    }

    Eigen::VectorXd localCoordinates(const Variable& other) const override
    {
        const auto xi = csym::local_coordinates(value, typed(other).value, kEpsilon);
        Eigen::VectorXd out(static_cast<Eigen::Index>(kTangentDim));
        for (std::size_t i = 0; i < kTangentDim; ++i) out[static_cast<Eigen::Index>(i)] = xi[i];
        return out;
    }

    Eigen::MatrixXd localCoordinatesJacobian(const Variable& other) const override
    {
        const auto n = static_cast<Eigen::Index>(kTangentDim);
        if constexpr (csym::lie<T>::identity_tangent)
        {
            return Eigen::MatrixXd::Identity(n, n);
        }
        else
        {
            const auto [xi, J] = LocalFn::template jacobian<1>(value, typed(other).value, kEpsilon);
            Eigen::MatrixXd out(n, n);
            for (std::size_t r = 0; r < kTangentDim; ++r)
                for (std::size_t c = 0; c < kTangentDim; ++c)
                    out(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) = J(r, c);
            return out;
        }
    }

  private:
    using LocalFn = csym::Function<detail::kLocalCoordinates, T, T, double>;

    static const VariableModel& typed(const Variable& other)
    {
        const auto* t = dynamic_cast<const VariableModel*>(&other);
        if (t == nullptr) throw std::invalid_argument("variable type mismatch");
        return *t;
    }
};

class Values
{
  public:
    Values() = default;
    Values(const Values& other) { *this = other; }
    Values& operator=(const Values& other)
    {
        if (this != &other)
        {
            vars_.clear();
            for (const auto& [k, v] : other.vars_) vars_.emplace(k, v->clone());
        }
        return *this;
    }
    Values(Values&&) noexcept = default;
    Values& operator=(Values&&) noexcept = default;

    template <class T>
    void insert(Key key, const T& value)
    {
        if (vars_.contains(key)) throw std::invalid_argument("duplicate key " + keyName(key));
        vars_.emplace(key, std::make_unique<VariableModel<T>>(value));
    }

    void insert(Key key, std::unique_ptr<Variable> v)
    {
        if (vars_.contains(key)) throw std::invalid_argument("duplicate key " + keyName(key));
        vars_.emplace(key, std::move(v));
    }

    template <class T>
    const T& at(Key key) const
    {
        const auto* t = dynamic_cast<const VariableModel<T>*>(&variable(key));
        if (t == nullptr) throw std::invalid_argument("wrong type requested for " + keyName(key));
        return t->value;
    }

    template <class T>
    void update(Key key, const T& value)
    {
        auto it = vars_.find(key);
        if (it == vars_.end()) throw std::out_of_range("no variable " + keyName(key));
        auto* t = dynamic_cast<VariableModel<T>*>(it->second.get());
        if (t == nullptr) throw std::invalid_argument("wrong type for " + keyName(key));
        t->value = value;
    }

    const Variable& variable(Key key) const
    {
        auto it = vars_.find(key);
        if (it == vars_.end()) throw std::out_of_range("no variable " + keyName(key));
        return *it->second;
    }
    Variable& variable(Key key)
    {
        auto it = vars_.find(key);
        if (it == vars_.end()) throw std::out_of_range("no variable " + keyName(key));
        return *it->second;
    }

    bool contains(Key key) const { return vars_.contains(key); }
    std::size_t size() const { return vars_.size(); }
    bool empty() const { return vars_.empty(); }
    void erase(Key key) { vars_.erase(key); }

    std::vector<Key> keys() const
    {
        std::vector<Key> out;
        out.reserve(vars_.size());
        for (const auto& [k, v] : vars_) out.push_back(k);
        return out;
    }

  private:
    std::map<Key, std::unique_ptr<Variable>> vars_;
};

}  // namespace factor_graph
