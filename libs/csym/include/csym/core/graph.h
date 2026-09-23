#pragma once

// Hash-consed expression DAG, built and manipulated during constant evaluation.
//
// Every node is interned: structurally identical expressions share one id, so common subexpressions
// are shared from the start. Operands always have smaller ids than the nodes that use them, which means
// ascending id order is a topological order. All passes (differentiation, substitution, lowering) are
// plain loops over ids with no recursion, which keeps them clear of the constexpr call-depth limit.
//
// Canonical forms (similar in spirit to a CAS like SymEngine):
//   Const  value
//   Var    first = variable index
//   Add    value = constant term, args = non-constant terms sorted by id; a term with a numeric coefficient
//          is a Mul carrying that coefficient
//   Mul    value = numeric coefficient, args = factors sorted by base id; a factor is either a base or
//          Pow(base, exponent); no factor is a Const or a Mul
//   Pow    args = {base, exponent}
//   Where  args = {cond, a, b}; comparisons (Lt, Le, Eq) produce 0 or 1

#include <algorithm>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>

#include "csym/core/buffer.h"
#include "csym/math/cmath.h"

namespace csym {

using id_t = std::uint32_t;
inline constexpr id_t kNone = 0xffffffffu;

enum class Op : std::uint8_t {
  Const,
  Var,
  Add,
  Mul,
  Pow,
  // unary
  Sin,
  Cos,
  Tan,
  Asin,
  Acos,
  Atan,
  Exp,
  Log,
  Tanh,
  Abs,
  Sign,
  SignNoZero,
  Floor,
  // binary
  Atan2,
  Min,
  Max,
  Lt,
  Le,
  Eq,
  // ternary
  Where,
};

constexpr bool is_unary(Op op) { return op >= Op::Sin && op <= Op::Floor; }
constexpr bool is_binary(Op op) { return op >= Op::Atan2 && op <= Op::Eq; }

struct Node {
  Op op;
  std::uint32_t first;  // offset into Graph::args (or the variable index for Var)
  std::uint32_t count;  // number of operands
  double value;         // Const value, Add constant term, Mul coefficient
  std::uint64_t hash;
};

class Graph {
 public:
  Buffer<Node> nodes;
  Buffer<id_t> args;
  std::uint32_t num_vars = 0;
  id_t zero = kNone, one = kNone, minus_one = kNone;

  constexpr Graph() : table_(4096, 0) {
    nodes.reserve(4096);
    args.reserve(8192);
    scratch_.reserve(64);
    terms_.reserve(1024);
    factors_.reserve(1024);
    ids_.reserve(1024);
    zero = constant(0.0);
    one = constant(1.0);
    minus_one = constant(-1.0);
  }

  // ---- accessors ------------------------------------------------------------------------------
  constexpr const Node& node(id_t i) const { return nodes[i]; }
  constexpr Op op(id_t i) const { return nodes[i].op; }
  constexpr bool is_const(id_t i) const { return nodes[i].op == Op::Const; }
  constexpr bool is_const(id_t i, double v) const { return is_const(i) && nodes[i].value == v; }
  constexpr double value(id_t i) const { return nodes[i].value; }
  constexpr std::uint32_t nargs(id_t i) const { return nodes[i].count; }
  constexpr id_t arg(id_t i, std::uint32_t k) const { return args[nodes[i].first + k]; }
  constexpr std::uint32_t var_index(id_t i) const { return nodes[i].first; }
  constexpr Buffer<id_t> arg_list(id_t i) const {
    const Node n = nodes[i];
    return Buffer<id_t>(args.begin() + n.first, args.begin() + n.first + n.count);
  }
  constexpr std::size_t size() const { return nodes.size(); }

  // ---- leaves ---------------------------------------------------------------------------------
  constexpr id_t constant(double v) {
    if (v == 0.0) v = 0.0;  // fold -0 into +0
    return intern(Op::Const, nullptr, 0, v, 0);
  }
  constexpr id_t variable() { return intern(Op::Var, nullptr, 0, 0.0, num_vars++); }

  // ---- arithmetic -----------------------------------------------------------------------------
  // Hot paths are allocation-free: short-lived std::vectors dominate the cost of constant evaluation,
  // so temporaries live on scratch stacks owned by the graph (terms_, factors_, ids_). Nested calls
  // (mul -> pow -> mul) push above their caller's frame and pop back, and everything is addressed by
  // index because a nested push may reallocate. Operand spans are fully consumed before anything is
  // pushed onto ids_, so callers may pass a span into ids_ itself.
  constexpr id_t add(std::span<const id_t> operands) {
    const std::size_t tb = terms_.size();
    double c = 0.0;
    for (const id_t o : operands) {
      const Node n = nodes[o];
      if (n.op == Op::Const) {
        c += n.value;
      } else if (n.op == Op::Add) {
        c += n.value;
        for (std::uint32_t k = 0; k < n.count; ++k) terms_.push_back(split_coefficient(args[n.first + k]));
      } else {
        terms_.push_back(split_coefficient(o));
      }
    }
    // insertion sort by term id (operand lists are short)
    for (std::size_t i = tb + 1; i < terms_.size(); ++i) {
      const auto t = terms_[i];
      std::size_t j = i;
      for (; j > tb && terms_[j - 1].first > t.first; --j) terms_[j] = terms_[j - 1];
      terms_[j] = t;
    }
    const std::size_t ob = ids_.size();
    for (std::size_t i = tb; i < terms_.size();) {
      double coef = 0.0;
      const id_t rest = terms_[i].first;
      for (; i < terms_.size() && terms_[i].first == rest; ++i) coef += terms_[i].second;
      if (coef != 0.0) ids_.push_back(scale(coef, rest));
    }
    terms_.resize(tb);
    const std::size_t count = ids_.size() - ob;
    id_t r;
    if (count == 0) r = constant(c);
    else if (count == 1 && c == 0.0) r = ids_[ob];
    else r = intern(Op::Add, ids_.data() + ob, count, c, 0);
    ids_.resize(ob);
    return r;
  }
  constexpr id_t add(std::initializer_list<id_t> l) { return add(std::span<const id_t>(l.begin(), l.size())); }
  constexpr id_t add(id_t a, id_t b) { return add({a, b}); }
  constexpr id_t sub(id_t a, id_t b) { return add(a, mul(minus_one, b)); }
  constexpr id_t neg(id_t a) { return mul(minus_one, a); }

  constexpr id_t mul(std::span<const id_t> operands) {
    const std::size_t fb = factors_.size();
    double c = 1.0;
    for (const id_t o : operands) {
      const Node n = nodes[o];
      if (n.op == Op::Const) {
        c *= n.value;
      } else if (n.op == Op::Mul) {
        c *= n.value;
        for (std::uint32_t k = 0; k < n.count; ++k) factors_.push_back(split_power(args[n.first + k]));
      } else {
        factors_.push_back(split_power(o));
      }
    }
    if (c == 0.0) {
      factors_.resize(fb);
      return zero;
    }
    // stable insertion sort by base id
    for (std::size_t i = fb + 1; i < factors_.size(); ++i) {
      const auto f = factors_[i];
      std::size_t j = i;
      for (; j > fb && factors_[j - 1].first > f.first; --j) factors_[j] = factors_[j - 1];
      factors_[j] = f;
    }
    const std::size_t ob = ids_.size();
    bool renormalize = false;
    for (std::size_t i = fb; i < factors_.size();) {
      const id_t base = factors_[i].first;
      std::size_t j = i + 1;
      while (j < factors_.size() && factors_[j].first == base) ++j;
      id_t e = factors_[i].second;
      if (j - i > 1) {
        // Numeric exponents (the common case) are summed directly; symbolic ones go through add().
        bool numeric = true;
        double sum = 0.0;
        for (std::size_t k = i; k < j && numeric; ++k) {
          numeric = is_const(factors_[k].second);
          if (numeric) sum += value(factors_[k].second);
        }
        if (numeric) {
          e = constant(sum);
        } else {
          const std::size_t eb = ids_.size();
          for (std::size_t k = i; k < j; ++k) ids_.push_back(factors_[k].second);
          e = add(std::span<const id_t>(ids_.data() + eb, j - i));
          ids_.resize(eb);
        }
      }
      i = j;
      if (is_const(e, 0.0)) continue;
      const id_t f = pow(base, e);  // may recurse into mul(); indices stay valid
      const Op fop = nodes[f].op;
      if (fop == Op::Const) {
        c *= nodes[f].value;
      } else {
        if (fop == Op::Mul) renormalize = true;
        ids_.push_back(f);
      }
    }
    factors_.resize(fb);
    id_t r;
    if (renormalize) {
      ids_.push_back(constant(c));
      r = mul(std::span<const id_t>(ids_.data() + ob, ids_.size() - ob));
    } else {
      const std::size_t count = ids_.size() - ob;
      if (c == 0.0) r = zero;
      else if (count == 0) r = constant(c);
      else if (count == 1 && c == 1.0) r = ids_[ob];
      else r = intern(Op::Mul, ids_.data() + ob, count, c, 0);
    }
    ids_.resize(ob);
    return r;
  }
  constexpr id_t mul(std::initializer_list<id_t> l) { return mul(std::span<const id_t>(l.begin(), l.size())); }
  constexpr id_t mul(id_t a, id_t b) { return mul({a, b}); }
  constexpr id_t div(id_t a, id_t b) { return mul(a, pow(b, minus_one)); }

  constexpr id_t pow(id_t b, id_t e) {
    if (is_const(e)) {
      const double ev = value(e);
      if (ev == 0.0) return one;
      if (ev == 1.0) return b;
      if (is_const(b)) return constant(cm::pow(value(b), ev));
      const Node bn = nodes[b];
      if (cm::is_integer(ev)) {
        // (x^a)^n = x^(a*n) for integer n
        if (bn.op == Op::Pow) return pow(args[bn.first], mul(args[bn.first + 1], e));
        // (c * x * y)^n = c^n * x^n * y^n for integer n
        if (bn.op == Op::Mul) {
          const std::size_t ob = ids_.size();
          ids_.push_back(constant(cm::pow(bn.value, ev)));
          for (std::uint32_t k = 0; k < bn.count; ++k) {
            const id_t p = pow(args[bn.first + k], e);
            ids_.push_back(p);
          }
          const id_t r = mul(std::span<const id_t>(ids_.data() + ob, ids_.size() - ob));
          ids_.resize(ob);
          return r;
        }
      }
    } else if (is_const(b, 1.0)) {
      return one;
    }
    const id_t ops[2] = {b, e};
    return intern(Op::Pow, ops, 2, 0.0, 0);
  }
  constexpr id_t pow(id_t b, double e) { return pow(b, constant(e)); }

  // ---- functions ------------------------------------------------------------------------------
  constexpr id_t unary(Op op, id_t a) {
    if (is_const(a)) return constant(eval_unary(op, value(a)));
    if (op == Op::Abs && nodes[a].op == Op::Abs) return a;
    return intern(op, &a, 1, 0.0, 0);
  }
  constexpr id_t binary(Op op, id_t a, id_t b) {
    if (is_const(a) && is_const(b)) return constant(eval_binary(op, value(a), value(b)));
    if (a == b) {
      if (op == Op::Min || op == Op::Max) return a;
      if (op == Op::Le || op == Op::Eq) return one;
      if (op == Op::Lt) return zero;
    }
    const id_t ops[2] = {a, b};
    return intern(op, ops, 2, 0.0, 0);
  }
  constexpr id_t where(id_t cond, id_t a, id_t b) {
    if (is_const(cond)) return value(cond) != 0.0 ? a : b;
    if (a == b) return a;
    const id_t ops[3] = {cond, a, b};
    return intern(Op::Where, ops, 3, 0.0, 0);
  }

  // Rebuilds node `i`'s operation over new operands, re-canonicalizing.
  // `a` must not point into the graph's own storage.
  constexpr id_t rebuild(id_t i, std::span<const id_t> a) {
    const Node n = nodes[i];
    switch (n.op) {
      case Op::Const:
      case Op::Var:
        return i;
      case Op::Add:
      case Op::Mul: {
        const std::size_t ob = ids_.size();
        for (const id_t x : a) ids_.push_back(x);
        ids_.push_back(constant(n.value));
        const std::span<const id_t> ops(ids_.data() + ob, ids_.size() - ob);
        const id_t r = n.op == Op::Add ? add(ops) : mul(ops);
        ids_.resize(ob);
        return r;
      }
      case Op::Pow:
        return pow(a[0], a[1]);
      case Op::Where:
        return where(a[0], a[1], a[2]);
      case Op::Sin:
      case Op::Cos:
      case Op::Tan:
      case Op::Asin:
      case Op::Acos:
      case Op::Atan:
      case Op::Exp:
      case Op::Log:
      case Op::Tanh:
      case Op::Abs:
      case Op::Sign:
      case Op::SignNoZero:
      case Op::Floor:
        return unary(n.op, a[0]);
      case Op::Atan2:
      case Op::Min:
      case Op::Max:
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
        return binary(n.op, a[0], a[1]);
    }
    return i;  // unreachable: every Op is handled above
  }

  // ---- differentiation ------------------------------------------------------------------------
  // d(root)/d(var) for each root.
  constexpr Buffer<id_t> diff(const Buffer<id_t>& roots, id_t var) {
    const std::pair<id_t, id_t> seed{var, one};
    return diff(roots, std::span<const std::pair<id_t, id_t>>(&seed, 1));
  }

  // Directional (forward-mode) derivative of each root, where each seed (variable, dv) gives the
  // variable's derivative along the direction; unseeded variables are constant. One loop in id order
  // over the reachable subgraph, each node's derivative built from its operands' derivatives.
  //
  // Seeding with a tangent direction (dv = column of storage_D_tangent) differentiates on the manifold
  // directly: intermediate results get their own tangent derivatives, shared across outputs, instead of
  // multiplying a storage Jacobian by storage_D_tangent at the end.
  constexpr Buffer<id_t> diff(const Buffer<id_t>& roots, std::span<const std::pair<id_t, id_t>> seeds) {
    const std::size_t n0 = nodes.size();
    Buffer<char> live = reachable(roots);
    Buffer<id_t> d(n0, zero);
    for (const auto& [v, dv] : seeds) d[v] = dv;
    Buffer<id_t> ts, fs;  // reused per node
    ts.reserve(64);
    fs.reserve(64);
    for (id_t i = 0; i < n0; ++i) {
      if (!live[i]) continue;
      const Node n = nodes[i];
      ts.clear();
      switch (n.op) {
        case Op::Const:
          break;
        case Op::Var:
          break;  // seeded above
        case Op::Add: {
          for (std::uint32_t k = 0; k < n.count; ++k) {
            const id_t dk = d[args[n.first + k]];
            if (dk != zero) ts.push_back(dk);
          }
          if (!ts.empty()) d[i] = ts.size() == 1 ? ts[0] : add(ts);
          break;
        }
        case Op::Mul: {
          for (std::uint32_t k = 0; k < n.count; ++k) {
            const id_t dk = d[args[n.first + k]];
            if (dk == zero) continue;
            fs.clear();
            fs.push_back(constant(n.value));
            fs.push_back(dk);
            for (std::uint32_t j = 0; j < n.count; ++j)
              if (j != k) fs.push_back(args[n.first + j]);
            ts.push_back(mul(fs));
          }
          if (!ts.empty()) d[i] = ts.size() == 1 ? ts[0] : add(ts);
          break;
        }
        case Op::Pow: {
          const id_t b = args[n.first], e = args[n.first + 1];
          const id_t db = d[b], de = d[e];
          // d(b^e) = e * b^(e-1) * db + b^e * log(b) * de
          if (db != zero) ts.push_back(mul({e, pow(b, add(e, constant(-1.0))), db}));
          if (de != zero) ts.push_back(mul({i, unary(Op::Log, b), de}));
          if (!ts.empty()) d[i] = ts.size() == 1 ? ts[0] : add(ts);
          break;
        }
        case Op::Where: {
          const id_t da = d[args[n.first + 1]], db = d[args[n.first + 2]];
          if (da != zero || db != zero) d[i] = where(args[n.first], da, db);
          break;
        }
        case Op::Sin:
        case Op::Cos:
        case Op::Tan:
        case Op::Asin:
        case Op::Acos:
        case Op::Atan:
        case Op::Exp:
        case Op::Log:
        case Op::Tanh:
        case Op::Abs:
        case Op::Sign:
        case Op::SignNoZero:
        case Op::Floor: {
          const id_t a = args[n.first];
          const id_t da = d[a];
          if (da != zero) {
            const id_t g = unary_derivative(n.op, i, a);
            if (g != zero) d[i] = mul(g, da);
          }
          break;
        }
        case Op::Atan2:
        case Op::Min:
        case Op::Max:
        case Op::Lt:
        case Op::Le:
        case Op::Eq: {
          const id_t a = args[n.first], b = args[n.first + 1];
          const id_t da = d[a], db = d[b];
          if (da == zero && db == zero) break;
          const auto [ga, gb] = binary_derivative(n.op, a, b);
          if (da != zero && ga != zero) ts.push_back(mul(ga, da));
          if (db != zero && gb != zero) ts.push_back(mul(gb, db));
          if (!ts.empty()) d[i] = ts.size() == 1 ? ts[0] : add(ts);
          break;
        }
      }
    }
    Buffer<id_t> out;
    out.reserve(roots.size());
    for (const id_t r : roots) out.push_back(d[r]);
    return out;
  }

  // ---- substitution ---------------------------------------------------------------------------
  // Replaces each `from[k]` with `to[k]` (by node id, typically variables) throughout `roots`.
  constexpr Buffer<id_t> substitute(const Buffer<id_t>& roots, const Buffer<id_t>& from,
                                         const Buffer<id_t>& to) {
    const std::size_t n0 = nodes.size();
    Buffer<char> live = reachable(roots);
    Buffer<id_t> m(n0, kNone);
    for (std::size_t k = 0; k < from.size(); ++k) m[from[k]] = to[k];
    Buffer<id_t> a;
    for (id_t i = 0; i < n0; ++i) {
      if (!live[i] || m[i] != kNone) continue;
      const Node n = nodes[i];
      if (n.op == Op::Const || n.op == Op::Var) {
        m[i] = i;
        continue;
      }
      a.clear();
      bool changed = false;
      for (std::uint32_t k = 0; k < n.count; ++k) {
        const id_t c = args[n.first + k];
        a.push_back(m[c]);
        changed |= (m[c] != c);
      }
      m[i] = changed ? rebuild(i, a) : i;
    }
    Buffer<id_t> out;
    for (const id_t r : roots) out.push_back(m[r]);
    return out;
  }

  // Marks every node reachable from `roots`.
  constexpr Buffer<char> reachable(const Buffer<id_t>& roots) const {
    Buffer<char> live(nodes.size(), 0);
    for (const id_t r : roots) live[r] = 1;
    for (id_t i = static_cast<id_t>(nodes.size()); i-- > 0;) {
      if (!live[i]) continue;
      const Node n = nodes[i];
      if (n.op == Op::Var) continue;
      for (std::uint32_t k = 0; k < n.count; ++k) live[args[n.first + k]] = 1;
    }
    return live;
  }

  static constexpr double eval_unary(Op op, double x) {
    switch (op) {
      case Op::Sin: return cm::sin(x);
      case Op::Cos: return cm::cos(x);
      case Op::Tan: return cm::tan(x);
      case Op::Asin: return cm::asin(x);
      case Op::Acos: return cm::acos(x);
      case Op::Atan: return cm::atan(x);
      case Op::Exp: return cm::exp(x);
      case Op::Log: return cm::log(x);
      case Op::Tanh: return cm::tanh(x);
      case Op::Abs: return cm::fabs(x);
      case Op::Sign: return cm::sign(x);
      case Op::SignNoZero: return cm::sign_no_zero(x);
      case Op::Floor: return cm::floor(x);
      case Op::Const:
      case Op::Var:
      case Op::Add:
      case Op::Mul:
      case Op::Pow:
      case Op::Atan2:
      case Op::Min:
      case Op::Max:
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
      case Op::Where:
        break;
    }
    return cm::nan;
  }
  static constexpr double eval_binary(Op op, double a, double b) {
    switch (op) {
      case Op::Atan2: return cm::atan2(a, b);
      case Op::Min: return cm::fmin(a, b);
      case Op::Max: return cm::fmax(a, b);
      case Op::Lt: return a < b ? 1.0 : 0.0;
      case Op::Le: return a <= b ? 1.0 : 0.0;
      case Op::Eq: return a == b ? 1.0 : 0.0;
      case Op::Const:
      case Op::Var:
      case Op::Add:
      case Op::Mul:
      case Op::Pow:
      case Op::Sin:
      case Op::Cos:
      case Op::Tan:
      case Op::Asin:
      case Op::Acos:
      case Op::Atan:
      case Op::Exp:
      case Op::Log:
      case Op::Tanh:
      case Op::Abs:
      case Op::Sign:
      case Op::SignNoZero:
      case Op::Floor:
      case Op::Where:
        break;
    }
    return cm::nan;
  }

 private:
  Buffer<id_t> table_;  // open addressing, 0 = empty, otherwise node id + 1
  Buffer<id_t> scratch_;                       // operand buffer for split/scale (intern copies out)
  Buffer<std::pair<id_t, double>> terms_;      // add(): (rest, coefficient) stack
  Buffer<std::pair<id_t, id_t>> factors_;      // mul(): (base, exponent) stack
  Buffer<id_t> ids_;                           // add()/mul()/pow() output stack

  // Derivative of f = op(a) with respect to a, where `self` is the node f.
  constexpr id_t unary_derivative(Op op, id_t self, id_t a) {
    switch (op) {
      case Op::Sin: return unary(Op::Cos, a);
      case Op::Cos: return neg(unary(Op::Sin, a));
      case Op::Tan: return add(one, pow(self, 2.0));  // 1 + tan^2
      case Op::Asin: return pow(sub(one, pow(a, 2.0)), -0.5);
      case Op::Acos: return neg(pow(sub(one, pow(a, 2.0)), -0.5));
      case Op::Atan: return pow(add(one, pow(a, 2.0)), -1.0);
      case Op::Exp: return self;
      case Op::Log: return pow(a, -1.0);
      case Op::Tanh: return sub(one, pow(self, 2.0));
      case Op::Abs: return unary(Op::Sign, a);
      case Op::Sign:
      case Op::SignNoZero:
      case Op::Floor:
        return zero;  // piecewise constant
      case Op::Const:
      case Op::Var:
      case Op::Add:
      case Op::Mul:
      case Op::Pow:
      case Op::Atan2:
      case Op::Min:
      case Op::Max:
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
      case Op::Where:
        break;  // not unary
    }
    return zero;
  }
  // Partials (d/da, d/db) of op(a, b).
  constexpr std::pair<id_t, id_t> binary_derivative(Op op, id_t a, id_t b) {
    switch (op) {
      case Op::Atan2: {  // atan2(y=a, x=b)
        const id_t inv = pow(add(pow(a, 2.0), pow(b, 2.0)), -1.0);
        return {mul(b, inv), neg(mul(a, inv))};
      }
      case Op::Min: {  // 0.5 * (1 + sign(b - a)), 0.5 * (1 + sign(a - b))
        const id_t half = constant(0.5);
        return {mul(half, add(one, unary(Op::Sign, sub(b, a)))), mul(half, add(one, unary(Op::Sign, sub(a, b))))};
      }
      case Op::Max: {
        const id_t half = constant(0.5);
        return {mul(half, add(one, unary(Op::Sign, sub(a, b)))), mul(half, add(one, unary(Op::Sign, sub(b, a))))};
      }
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
        return {zero, zero};  // comparisons are piecewise constant
      case Op::Const:
      case Op::Var:
      case Op::Add:
      case Op::Mul:
      case Op::Pow:
      case Op::Sin:
      case Op::Cos:
      case Op::Tan:
      case Op::Asin:
      case Op::Acos:
      case Op::Atan:
      case Op::Exp:
      case Op::Log:
      case Op::Tanh:
      case Op::Abs:
      case Op::Sign:
      case Op::SignNoZero:
      case Op::Floor:
      case Op::Where:
        break;  // not binary
    }
    return {zero, zero};
  }

  // Term -> (coefficient, rest) for Add canonicalization.
  constexpr std::pair<id_t, double> split_coefficient(id_t t) {
    const Node n = nodes[t];
    if (n.op != Op::Mul || n.value == 1.0) return {t, 1.0};
    if (n.count == 1) return {args[n.first], n.value};
    scratch_.assign(args.begin() + n.first, args.begin() + n.first + n.count);
    return {intern(Op::Mul, scratch_.data(), scratch_.size(), 1.0, 0), n.value};
  }
  // coefficient * rest, in canonical form.
  constexpr id_t scale(double coef, id_t rest) {
    if (coef == 1.0) return rest;
    const Node n = nodes[rest];
    if (n.op == Op::Mul && n.value == 1.0) {
      scratch_.assign(args.begin() + n.first, args.begin() + n.first + n.count);
      return intern(Op::Mul, scratch_.data(), scratch_.size(), coef, 0);
    }
    return intern(Op::Mul, &rest, 1, coef, 0);
  }
  // Factor -> (base, exponent) for Mul canonicalization.
  constexpr std::pair<id_t, id_t> split_power(id_t f) {
    const Node n = nodes[f];
    if (n.op == Op::Pow) return {args[n.first], args[n.first + 1]};
    return {f, one};
  }

  static constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xbf58476d1ce4e5b9ull;
    return h ^ (h >> 31);
  }

  constexpr id_t intern(Op op, const id_t* a, std::size_t count, double value, std::uint32_t aux) {
    std::uint64_t h = mix(static_cast<std::uint64_t>(op) + 1, std::bit_cast<std::uint64_t>(value));
    h = mix(h, aux);
    for (std::size_t k = 0; k < count; ++k) h = mix(h, a[k]);
    if ((nodes.size() + 1) * 2 > table_.size()) grow();
    const std::size_t mask = table_.size() - 1;
    for (std::size_t slot = h & mask;; slot = (slot + 1) & mask) {
      const id_t e = table_[slot];
      if (e == 0) {
        const auto id = static_cast<id_t>(nodes.size());
        const auto first = static_cast<std::uint32_t>(op == Op::Var ? aux : args.size());
        for (std::size_t k = 0; k < count; ++k) args.push_back(a[k]);
        nodes.push_back(Node{op, first, static_cast<std::uint32_t>(count), value, h});
        table_[slot] = id + 1;
        return id;
      }
      const Node& n = nodes[e - 1];
      if (n.hash != h || n.op != op || n.count != count ||
          std::bit_cast<std::uint64_t>(n.value) != std::bit_cast<std::uint64_t>(value))
        continue;
      if (op == Op::Var) {
        if (n.first == aux) return e - 1;
        continue;
      }
      bool same = true;
      for (std::size_t k = 0; k < count && same; ++k) same = args[n.first + k] == a[k];
      if (same) return e - 1;
    }
  }

  constexpr void grow() {
    Buffer<id_t> t(table_.size() * 2, 0);
    const std::size_t mask = t.size() - 1;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      std::size_t slot = nodes[i].hash & mask;
      while (t[slot]) slot = (slot + 1) & mask;
      t[slot] = static_cast<id_t>(i + 1);
    }
    table_ = std::move(t);
  }
};

}  // namespace csym
