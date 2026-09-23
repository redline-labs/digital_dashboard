#pragma once

// Expr: the symbolic scalar that user functions are traced with.
//
// An Expr is either attached to a Graph (a node id) or a detached numeric constant. Detached constants
// let generic code write `T(0)`, `T(1)`, `2.0 * x` and so on without knowing about the graph; they are
// materialized as Const nodes the first time they meet an attached Expr.
//
// Comparisons return an Expr holding 0 or 1, so symbolic code cannot branch on them. Use csym::where().

#include <concepts>
#include <type_traits>

#include "csym/core/batch_fwd.h"
#include "csym/core/graph.h"
#include "csym/math/cmath.h"

namespace csym {

class Expr;
constexpr Graph* pick(const Expr& a, const Expr& b);

class Expr {
 public:
  Graph* graph = nullptr;
  id_t id = kNone;
  double constant_value = 0.0;

  constexpr Expr() = default;
  template <class A>
    requires std::is_arithmetic_v<A>
  constexpr Expr(A x) : constant_value(static_cast<double>(x)) {}  // NOLINT: implicit by design
  constexpr Expr(Graph* g, id_t i) : graph(g), id(i) {}

  constexpr bool is_constant() const { return graph == nullptr || graph->is_const(id); }
  constexpr double value() const { return graph ? graph->value(id) : constant_value; }
  // Node id of this expression in `g`, materializing a detached constant if needed.
  constexpr id_t node(Graph& g) const { return graph ? id : g.constant(constant_value); }

  // ---- arithmetic (hidden friends so that mixed Expr/number calls convert implicitly) ---------
  friend constexpr Expr operator+(const Expr& a, const Expr& b) {
    if (!a.graph && !b.graph) return a.constant_value + b.constant_value;
    Graph& g = *pick(a, b);
    return {&g, g.add(a.node(g), b.node(g))};
  }
  friend constexpr Expr operator-(const Expr& a, const Expr& b) {
    if (!a.graph && !b.graph) return a.constant_value - b.constant_value;
    Graph& g = *pick(a, b);
    return {&g, g.sub(a.node(g), b.node(g))};
  }
  friend constexpr Expr operator*(const Expr& a, const Expr& b) {
    if (!a.graph && !b.graph) return a.constant_value * b.constant_value;
    Graph& g = *pick(a, b);
    return {&g, g.mul(a.node(g), b.node(g))};
  }
  friend constexpr Expr operator/(const Expr& a, const Expr& b) {
    if (!a.graph && !b.graph) return a.constant_value / b.constant_value;
    Graph& g = *pick(a, b);
    return {&g, g.div(a.node(g), b.node(g))};
  }
  friend constexpr Expr operator-(const Expr& a) {
    if (!a.graph) return -a.constant_value;
    return {a.graph, a.graph->neg(a.id)};
  }
  friend constexpr Expr operator+(const Expr& a) { return a; }
  constexpr Expr& operator+=(const Expr& b) { return *this = *this + b; }
  constexpr Expr& operator-=(const Expr& b) { return *this = *this - b; }
  constexpr Expr& operator*=(const Expr& b) { return *this = *this * b; }
  constexpr Expr& operator/=(const Expr& b) { return *this = *this / b; }

  friend constexpr Expr operator<(const Expr& a, const Expr& b) { return binary(Op::Lt, a, b); }
  friend constexpr Expr operator<=(const Expr& a, const Expr& b) { return binary(Op::Le, a, b); }
  friend constexpr Expr operator>(const Expr& a, const Expr& b) { return binary(Op::Lt, b, a); }
  friend constexpr Expr operator>=(const Expr& a, const Expr& b) { return binary(Op::Le, b, a); }

  // ---- functions ------------------------------------------------------------------------------
  static constexpr Expr unary(Op op, const Expr& a) {
    if (!a.graph) return Graph::eval_unary(op, a.constant_value);
    return {a.graph, a.graph->unary(op, a.id)};
  }
  static constexpr Expr binary(Op op, const Expr& a, const Expr& b) {
    if (!a.graph && !b.graph) return Graph::eval_binary(op, a.constant_value, b.constant_value);
    Graph& g = *pick(a, b);
    return {&g, g.binary(op, a.node(g), b.node(g))};
  }

};

// The graph shared by two operands (at least one is attached). This is a namespace-scope function on
// purpose: clang 21's bytecode constant interpreter (-fexperimental-new-constant-interpreter) mis-evaluates
// the same code written as a member or static member function of Expr.
constexpr Graph* pick(const Expr& a, const Expr& b) { return a.graph ? a.graph : b.graph; }

constexpr Expr sin(const Expr& x) { return Expr::unary(Op::Sin, x); }
constexpr Expr cos(const Expr& x) { return Expr::unary(Op::Cos, x); }
constexpr Expr tan(const Expr& x) { return Expr::unary(Op::Tan, x); }
constexpr Expr asin(const Expr& x) { return Expr::unary(Op::Asin, x); }
constexpr Expr acos(const Expr& x) { return Expr::unary(Op::Acos, x); }
constexpr Expr atan(const Expr& x) { return Expr::unary(Op::Atan, x); }
constexpr Expr exp(const Expr& x) { return Expr::unary(Op::Exp, x); }
constexpr Expr log(const Expr& x) { return Expr::unary(Op::Log, x); }
constexpr Expr tanh(const Expr& x) { return Expr::unary(Op::Tanh, x); }
constexpr Expr abs(const Expr& x) { return Expr::unary(Op::Abs, x); }
constexpr Expr sign(const Expr& x) { return Expr::unary(Op::Sign, x); }
constexpr Expr sign_no_zero(const Expr& x) { return Expr::unary(Op::SignNoZero, x); }
constexpr Expr floor(const Expr& x) { return Expr::unary(Op::Floor, x); }
constexpr Expr atan2(const Expr& y, const Expr& x) { return Expr::binary(Op::Atan2, y, x); }
constexpr Expr min(const Expr& a, const Expr& b) { return Expr::binary(Op::Min, a, b); }
constexpr Expr max(const Expr& a, const Expr& b) { return Expr::binary(Op::Max, a, b); }
constexpr Expr eq(const Expr& a, const Expr& b) { return Expr::binary(Op::Eq, a, b); }
constexpr Expr copysign_no_zero(const Expr& a, const Expr& b) { return abs(a) * sign_no_zero(b); }

constexpr Expr pow(const Expr& b, const Expr& e) {
  if (!b.graph && !e.graph) return cm::pow(b.constant_value, e.constant_value);
  Graph& g = *pick(b, e);
  return {&g, g.pow(b.node(g), e.node(g))};
}
constexpr Expr sqrt(const Expr& x) { return pow(x, 0.5); }

// cond ? a : b, with cond nonzero meaning true. Both branches are always evaluated.
constexpr Expr where(const Expr& cond, const Expr& a, const Expr& b) {
  if (!cond.graph) return cond.constant_value != 0.0 ? a : b;
  Graph& g = *cond.graph;
  return {&g, g.where(cond.id, a.node(g), b.node(g))};
}

// Scalar concept covering numeric, batched (Batch<T, W>) and symbolic scalars.
template <class T>
concept Scalar = std::floating_point<T> || std::same_as<T, Expr> || is_batch_v<T>;
// Scalars a Function can be evaluated with.
template <class T>
concept NumericScalar = std::floating_point<T> || is_batch_v<T>;

template <std::floating_point T>
constexpr T eq(T a, T b) { return a == b ? T(1) : T(0); }

// ---- helpers usable for any Scalar ---------------------------------------------------------------
template <Scalar T>
constexpr T square(const T& x) { return x * x; }

// atan2 that stays finite (and differentiable) at x == 0 by nudging x away from zero by epsilon.
template <Scalar T>
constexpr T atan2_safe(const T& y, const T& x, const T& epsilon) {
  return atan2(y, x + copysign_no_zero(epsilon, x));
}

}  // namespace csym
