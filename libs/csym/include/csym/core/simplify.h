#pragma once

// Graph-level simplification passes.
//
//   expand_and_factor
//            Distributes products (and small positive integer powers) of sums so that like terms
//            combine and cancel, then re-factors with multivariate Horner (repeatedly pulling out the
//            factor shared by the most terms: a*x + b*x + c -> x*(a + b) + c). Only the polynomial region
//            (Add/Mul/Pow reachable from the roots without passing through a function) is touched;
//            function applications are atoms whose arguments are never rewritten, so they stay shared.
//   optimize For each root, keeps the cheaper of the original and the expanded+factored form, where the
//            cost of a form is the number of operations it adds on top of what earlier roots already
//            compute (so shared subexpressions are free).
//
// Like graph.h, these avoid short-lived allocations (they dominate constant-evaluation time): buffers
// are hoisted out of loops and reused, term lists are flat pools rather than vectors of vectors, and
// expansion works on its own polynomial representation so intermediate results are never interned.


#include "csym/core/buffer.h"
#include "csym/core/graph.h"

namespace csym {

namespace detail {

// Nodes reachable from `roots` through Add/Mul/Pow edges only.
constexpr Buffer<char> polynomial_region(const Graph& g, const Buffer<id_t>& roots) {
  Buffer<char> live(g.size(), 0);
  for (const id_t r : roots) live[r] = 1;
  for (id_t i = static_cast<id_t>(g.size()); i-- > 0;) {
    if (!live[i]) continue;
    const Op op = g.op(i);
    if (op != Op::Add && op != Op::Mul && op != Op::Pow) continue;
    for (std::uint32_t k = 0; k < g.nargs(i); ++k) live[g.arg(i, k)] = 1;
  }
  return live;
}

// Multivariate Horner factoring over a flat pool of terms.
class Horner {
 public:
  struct Term {
    double coef;
    std::uint32_t begin, end;  // range in pool_ (exponent 0 = factor removed)
  };

  constexpr explicit Horner(Graph& g) : g_(g) {}

  constexpr void clear() {
    pool_.clear();
    terms_.clear();
  }
  constexpr void add_term(double coef, const std::pair<id_t, double>* factors, std::uint32_t n) {
    const auto b = static_cast<std::uint32_t>(pool_.size());
    for (std::uint32_t k = 0; k < n; ++k) pool_.push_back(factors[k]);
    terms_.push_back(Term{coef, b, b + n});
  }
  constexpr id_t result() { return factor(terms_); }

 private:
  Graph& g_;
  Buffer<std::pair<id_t, double>> pool_;  // (base, numeric exponent)
  Buffer<Term> terms_;
  struct Count {
    id_t base;
    bool negative;
    std::uint32_t n;
  };
  Buffer<Count> counts_;
  Buffer<id_t> buf_;

  constexpr id_t compose(const Term& t) {
    buf_.clear();
    buf_.push_back(g_.constant(t.coef));
    for (std::uint32_t k = t.begin; k < t.end; ++k) {
      const auto [b, e] = pool_[k];
      if (e == 0.0) continue;
      const id_t f = e == 1.0 ? b : g_.pow(b, e);
      buf_.push_back(f);
    }
    return g_.mul(buf_);
  }

  constexpr id_t sum_of(const Buffer<Term>& terms) {
    Buffer<id_t> ts;
    ts.reserve(terms.size());
    for (const auto& t : terms) ts.push_back(compose(t));
    return g_.add(ts);
  }

  constexpr id_t factor(const Buffer<Term>& terms) {
    if (terms.empty()) return g_.zero;
    if (terms.size() == 1) return compose(terms[0]);
    // Most common (base, exponent sign) across terms. A base can be factored out of the terms where
    // its exponent has the same sign: x^2*a + x^3*b -> x^2*(a + x*b), n^-2*a + n^-1.5*b ->
    // n^-1.5*(n^-0.5*a + b).
    counts_.clear();
    for (const auto& t : terms)
      for (std::uint32_t k = t.begin; k < t.end; ++k) {
        const auto [b, e] = pool_[k];
        if (e == 0.0) continue;
        const bool neg = e < 0.0;
        std::size_t c = 0;
        while (c < counts_.size() && (counts_[c].base != b || counts_[c].negative != neg)) ++c;
        if (c == counts_.size()) counts_.push_back(Count{b, neg, 1});
        else ++counts_[c].n;
      }
    // Factor out compound atoms (function results, shared subexpressions) before plain variables:
    // grouping by the expensive pieces is what keeps them computed once, e.g.
    // f'(u)*(a*x + b*y) rather than x*(a*f'(u)) + y*(b*f'(u)).
    Count best{kNone, false, 0};
    std::uint32_t best_score = 0;
    for (const Count& c : counts_) {
      if (c.n < 2) continue;
      const std::uint32_t score = c.n * (g_.op(c.base) == Op::Var ? 1u : 4u);
      if (score > best_score || (score == best_score && c.base < best.base)) {
        best = c;
        best_score = score;
      }
    }
    if (best.base == kNone) return sum_of(terms);

    auto matches = [&](const std::pair<id_t, double>& f) {
      return f.first == best.base && f.second != 0.0 && (f.second < 0.0) == best.negative;
    };
    double pulled = best.negative ? -1e300 : 1e300;  // exponent closest to zero
    for (const auto& t : terms)
      for (std::uint32_t k = t.begin; k < t.end; ++k)
        if (matches(pool_[k]))
          pulled = best.negative ? (pool_[k].second > pulled ? pool_[k].second : pulled)
                                 : (pool_[k].second < pulled ? pool_[k].second : pulled);
    Buffer<Term> with, without;
    for (const auto& t : terms) {
      bool has = false;
      for (std::uint32_t k = t.begin; k < t.end && !has; ++k)
        if (matches(pool_[k])) {
          has = true;
          pool_[k].second -= pulled;
        }
      (has ? with : without).push_back(t);
    }
    const double min_e = pulled;
    const id_t inner = factor(with);
    const id_t head = min_e == 1.0 ? best.base : g_.pow(best.base, min_e);
    const id_t factored = g_.mul(head, inner);
    if (without.empty()) return factored;
    const id_t rest = factor(without);
    return g_.add(factored, rest);
  }
};

constexpr std::size_t power_cost(double e) {
  if (e < 0) e = -e;
  if (cm::is_integer(e)) {
    std::size_t c = 0;
    for (auto k = static_cast<std::uint64_t>(e); k > 1; k >>= 1) c += 1 + (k & 1);
    return c;
  }
  if (cm::is_integer(2 * e)) return 1 + (e > 1 ? static_cast<std::size_t>(e) : 0);
  return 1;
}

// Operation-count estimate of a node on its own (operands are assumed available).
constexpr std::size_t local_cost(const Graph& g, id_t i) {
  const Node n = g.node(i);
  switch (n.op) {
    case Op::Const:
    case Op::Var: return 0;
    case Op::Add: {
      std::size_t c = n.count - 1 + (n.value != 0.0);
      for (std::uint32_t k = 0; k < n.count; ++k) {
        const id_t t = g.arg(i, k);
        if (g.op(t) == Op::Mul && g.value(t) != 1.0 && g.value(t) != -1.0) ++c;
      }
      return c;
    }
    case Op::Mul: {
      std::size_t c = n.count - 1;
      for (std::uint32_t k = 0; k < n.count; ++k) {
        const id_t f = g.arg(i, k);
        if (g.op(f) == Op::Pow && g.is_const(g.arg(f, 1))) c += power_cost(g.value(g.arg(f, 1)));
      }
      return c;
    }
    case Op::Pow: {
      const id_t e = g.arg(i, 1);
      return g.is_const(e) ? power_cost(g.value(e)) + (g.value(e) < 0) : 1;
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
    case Op::Floor:
    case Op::Atan2:
    case Op::Min:
    case Op::Max:
    case Op::Lt:
    case Op::Le:
    case Op::Eq:
    case Op::Where: return 1;
  }
  return 1;
}

// Marginal cost of roots given what is already covered. Buffers are reused across calls.
class CostModel {
 public:
  constexpr explicit CostModel(const Graph& g) : g_(g), covered_(g.size(), 0) {
    stack_.reserve(256);
    visited_.reserve(256);
  }

  // Ops needed for `root` beyond the covered nodes; marks them covered if `commit`.
  constexpr std::size_t marginal(id_t root, bool commit) {
    stack_.clear();
    visited_.clear();
    stack_.push_back(root);
    std::size_t cost = 0;
    while (!stack_.empty()) {
      const id_t i = stack_.back();
      stack_.pop_back();
      if (covered_[i]) continue;
      covered_[i] = 1;
      visited_.push_back(i);
      cost += local_cost(g_, i);
      const Node n = g_.node(i);
      if (n.op == Op::Var) continue;
      for (std::uint32_t k = 0; k < n.count; ++k) {
        const id_t c = g_.arg(i, k);
        if (n.op == Op::Mul && g_.op(c) == Op::Pow && g_.is_const(g_.arg(c, 1))) {
          stack_.push_back(g_.arg(c, 0));  // numeric powers are lowered inline
        } else if (n.op == Op::Add && g_.op(c) == Op::Mul) {
          cost += local_cost(g_, c);  // Add terms are lowered inline too
          for (std::uint32_t j = 0; j < g_.nargs(c); ++j) {
            const id_t f = g_.arg(c, j);
            stack_.push_back(g_.op(f) == Op::Pow && g_.is_const(g_.arg(f, 1)) ? g_.arg(f, 0) : f);
          }
        } else {
          stack_.push_back(c);
        }
      }
    }
    if (!commit)
      for (const id_t i : visited_) covered_[i] = 0;
    return cost;
  }

 private:
  const Graph& g_;
  Buffer<char> covered_;
  Buffer<id_t> stack_, visited_;
};

}  // namespace detail

namespace detail {

// Polynomial view of the graph's arithmetic region, used to expand and re-factor expressions without
// interning intermediate results. A polynomial is a list of monomials; a monomial is a coefficient
// times a product of (atom, exponent) factors sorted by atom id. Atoms are graph nodes: variables,
// function applications, and anything that would expand past `max_terms`.
class PolyExpander {
 public:
  struct Mono {
    double coef;
    std::uint32_t begin, len;  // factor range in fpool_
  };
  struct Poly {
    std::uint32_t begin, len;  // monomial range in mpool_
  };

  constexpr PolyExpander(Graph& g, std::size_t max_terms) : g_(g), max_terms_(max_terms) {}

  // Expanded + Horner-factored form of each root (roots outside the arithmetic region are unchanged).
  constexpr Buffer<id_t> run(const Buffer<id_t>& roots) {
    const std::size_t n0 = g_.size();
    const Buffer<char> live = polynomial_region(g_, roots);
    poly_ = Buffer<Poly>(n0, Poly{0, 0});
    materialized_ = Buffer<id_t>(n0, kNone);
    for (id_t i = 0; i < n0; ++i)
      if (live[i]) poly_[i] = build(i);
    Buffer<id_t> out;
    out.reserve(roots.size());
    for (const id_t r : roots) out.push_back(materialize(r));
    return out;
  }

 private:
  Graph& g_;
  std::size_t max_terms_;
  Buffer<std::pair<id_t, double>> fpool_;  // factors
  Buffer<Mono> mpool_;                     // monomials
  Buffer<Poly> poly_;                      // per node
  Buffer<Mono> tmp_;                       // scratch monomials during a product
  Buffer<id_t> materialized_;              // per node: factored graph node, once built
  Horner horner_{g_};
  Buffer<std::uint32_t> order_;
  Buffer<std::pair<id_t, double>> atoms_;
  Buffer<id_t> terms_, vf_;

  // Graph node for node i's polynomial, cached. Monomials are grouped by their atom part (every factor
  // that is not a plain variable); within a group the variable part is a polynomial, built as one
  // canonical sum scaled to leading coefficient 1 so the same form is shared wherever it appears (e.g.
  // the components of a quaternion product). Horner then factors common atoms out of the groups.
  constexpr id_t materialize(id_t i) {
    if (materialized_[i] != kNone) return materialized_[i];
    const Poly p = poly_[i];
    // order monomial indices by atom part
    order_.clear();
    for (std::uint32_t m = p.begin; m < p.begin + p.len; ++m) order_.push_back(m);
    for (std::size_t x = 1; x < order_.size(); ++x) {
      const std::uint32_t v = order_[x];
      std::size_t y = x;
      for (; y > 0 && atoms_less(mpool_[v], mpool_[order_[y - 1]]); --y) order_[y] = order_[y - 1];
      order_[y] = v;
    }
    horner_.clear();
    for (std::size_t x = 0; x < order_.size();) {
      std::size_t y = x + 1;
      while (y < order_.size() && atoms_equal(mpool_[order_[x]], mpool_[order_[y]])) ++y;
      const Mono head = mpool_[order_[x]];
      atoms_.clear();
      for (std::uint32_t k = head.begin; k < head.begin + head.len; ++k)
        if (!is_variable_factor(fpool_[k])) atoms_.push_back(fpool_[k]);
      if (y - x == 1) {
        horner_.add_term(head.coef, fpool_.data() + head.begin, head.len);
      } else {
        // Variable polynomial of the group, normalized to leading coefficient 1.
        const double lead = head.coef;
        terms_.clear();
        for (std::size_t z = x; z < y; ++z) {
          const Mono m = mpool_[order_[z]];
          vf_.clear();
          vf_.push_back(g_.constant(m.coef / lead));
          for (std::uint32_t k = m.begin; k < m.begin + m.len; ++k)
            if (is_variable_factor(fpool_[k])) {
              const auto [b, e] = fpool_[k];
              vf_.push_back(e == 1.0 ? b : g_.pow(b, e));
            }
          terms_.push_back(g_.mul(vf_));
        }
        const id_t vp = g_.add(terms_);
        insert_sorted(atoms_, {vp, 1.0});
        horner_.add_term(lead, atoms_.data(), static_cast<std::uint32_t>(atoms_.size()));
      }
      x = y;
    }
    return materialized_[i] = horner_.result();
  }

  // The child as a polynomial if it is a single monomial; otherwise as the factors of its materialized
  // form (so common atoms stay visible to the enclosing expression).
  constexpr Poly as_factor(id_t c) {
    if (poly_[c].len == 1) return poly_[c];
    const id_t m = materialize(c);
    if (g_.op(m) != Op::Mul) return atom(m, 1.0);
    const auto fb = static_cast<std::uint32_t>(fpool_.size());
    for (std::uint32_t k = 0; k < g_.nargs(m); ++k) {
      const id_t f = g_.arg(m, k);
      if (g_.op(f) == Op::Pow && g_.is_const(g_.arg(f, 1))) fpool_.emplace_back(g_.arg(f, 0), g_.value(g_.arg(f, 1)));
      else fpool_.emplace_back(f, 1.0);
    }
    // Mul factors are sorted by base id already (canonical form), as monomials require.
    const auto mb = static_cast<std::uint32_t>(mpool_.size());
    mpool_.push_back(Mono{g_.value(m), fb, static_cast<std::uint32_t>(fpool_.size()) - fb});
    return Poly{mb, 1};
  }

  constexpr bool is_variable_factor(const std::pair<id_t, double>& f) const {
    return g_.op(f.first) == Op::Var && f.second > 0.0 && cm::is_integer(f.second);
  }
  // Compare the atom parts (non-variable factors) of two monomials.
  constexpr int compare_atoms(const Mono& a, const Mono& b) const {
    std::uint32_t i = a.begin, j = b.begin;
    const std::uint32_t ie = a.begin + a.len, je = b.begin + b.len;
    while (true) {
      while (i < ie && is_variable_factor(fpool_[i])) ++i;
      while (j < je && is_variable_factor(fpool_[j])) ++j;
      if (i == ie || j == je) return (i == ie) == (j == je) ? 0 : (i == ie ? -1 : 1);
      if (fpool_[i].first != fpool_[j].first) return fpool_[i].first < fpool_[j].first ? -1 : 1;
      if (fpool_[i].second != fpool_[j].second) return fpool_[i].second < fpool_[j].second ? -1 : 1;
      ++i;
      ++j;
    }
  }
  constexpr bool atoms_less(const Mono& a, const Mono& b) const { return compare_atoms(a, b) < 0; }
  constexpr bool atoms_equal(const Mono& a, const Mono& b) const { return compare_atoms(a, b) == 0; }
  static constexpr void insert_sorted(Buffer<std::pair<id_t, double>>& fs, std::pair<id_t, double> f) {
    fs.push_back(f);
    std::size_t k = fs.size() - 1;
    for (; k > 0 && fs[k - 1].first > f.first; --k) fs[k] = fs[k - 1];
    fs[k] = f;
  }

  constexpr Poly atom(id_t base, double exponent) {
    const auto fb = static_cast<std::uint32_t>(fpool_.size());
    fpool_.emplace_back(base, exponent);
    const auto mb = static_cast<std::uint32_t>(mpool_.size());
    mpool_.push_back(Mono{1.0, fb, 1});
    return Poly{mb, 1};
  }
  constexpr Poly constant(double c) {
    const auto mb = static_cast<std::uint32_t>(mpool_.size());
    mpool_.push_back(Mono{c, static_cast<std::uint32_t>(fpool_.size()), 0});
    return Poly{mb, 1};
  }

  // Monomial product: merge two sorted factor lists into fpool_.
  constexpr Mono times(const Mono& a, const Mono& b) {
    const auto begin = static_cast<std::uint32_t>(fpool_.size());
    std::uint32_t i = a.begin, j = b.begin;
    const std::uint32_t ie = a.begin + a.len, je = b.begin + b.len;
    while (i < ie || j < je) {
      if (j >= je || (i < ie && fpool_[i].first < fpool_[j].first)) {
        fpool_.push_back(fpool_[i++]);
      } else if (i >= ie || fpool_[j].first < fpool_[i].first) {
        fpool_.push_back(fpool_[j++]);
      } else {
        const double e = fpool_[i].second + fpool_[j].second;
        const id_t base = fpool_[i].first;
        ++i;
        ++j;
        if (e != 0.0) fpool_.emplace_back(base, e);
      }
    }
    return Mono{a.coef * b.coef, begin, static_cast<std::uint32_t>(fpool_.size()) - begin};
  }

  constexpr bool same_factors(const Mono& a, const Mono& b) const {
    if (a.len != b.len) return false;
    for (std::uint32_t k = 0; k < a.len; ++k)
      if (fpool_[a.begin + k].first != fpool_[b.begin + k].first ||
          fpool_[a.begin + k].second != fpool_[b.begin + k].second)
        return false;
    return true;
  }
  constexpr bool less_factors(const Mono& a, const Mono& b) const {
    const std::uint32_t n = a.len < b.len ? a.len : b.len;
    for (std::uint32_t k = 0; k < n; ++k) {
      const auto& x = fpool_[a.begin + k];
      const auto& y = fpool_[b.begin + k];
      if (x.first != y.first) return x.first < y.first;
      if (x.second != y.second) return x.second < y.second;
    }
    return a.len < b.len;
  }

  // Sorts and merges like monomials of tmp_, appends the result to mpool_.
  constexpr Poly collect() {
    for (std::size_t i = 1; i < tmp_.size(); ++i) {
      const Mono m = tmp_[i];
      std::size_t j = i;
      for (; j > 0 && less_factors(m, tmp_[j - 1]); --j) tmp_[j] = tmp_[j - 1];
      tmp_[j] = m;
    }
    const auto mb = static_cast<std::uint32_t>(mpool_.size());
    for (std::size_t i = 0; i < tmp_.size();) {
      Mono m = tmp_[i++];
      while (i < tmp_.size() && same_factors(m, tmp_[i])) m.coef += tmp_[i++].coef;
      if (m.coef != 0.0) mpool_.push_back(m);
    }
    return Poly{mb, static_cast<std::uint32_t>(mpool_.size()) - mb};
  }

  constexpr Poly product(const Poly& a, const Poly& b) {
    tmp_.clear();
    for (std::uint32_t i = a.begin; i < a.begin + a.len; ++i)
      for (std::uint32_t j = b.begin; j < b.begin + b.len; ++j) tmp_.push_back(times(mpool_[i], mpool_[j]));
    return collect();
  }

  constexpr Poly build(id_t i) {
    const Node n = g_.node(i);
    switch (n.op) {
      case Op::Const: return constant(n.value);
      case Op::Add: {
        std::size_t total = 1;
        for (std::uint32_t k = 0; k < n.count; ++k) total += poly_[g_.arg(i, k)].len;
        const bool big = total > max_terms_;
        tmp_.clear();
        if (n.value != 0.0) tmp_.push_back(Mono{n.value, 0, 0});
        for (std::uint32_t k = 0; k < n.count; ++k) {
          // Too many terms: keep each (multi-term) summand as one factored atom.
          const Poly p = big ? as_factor(g_.arg(i, k)) : poly_[g_.arg(i, k)];
          for (std::uint32_t m = p.begin; m < p.begin + p.len; ++m) tmp_.push_back(mpool_[m]);
        }
        return collect();
      }
      case Op::Mul: {
        Poly acc = constant(n.value);
        for (std::uint32_t k = 0; k < n.count; ++k) {
          const id_t c = g_.arg(i, k);
          // Distribute while the product stays small; otherwise multiply by the factored child.
          const Poly f = static_cast<std::size_t>(acc.len) * poly_[c].len > max_terms_ ? as_factor(c) : poly_[c];
          acc = product(acc, f);
        }
        return acc;
      }
      case Op::Pow: {
        const id_t b = g_.arg(i, 0), e = g_.arg(i, 1);
        if (!g_.is_const(e)) return atom(i, 1.0);
        const double ev = g_.value(e);
        const Poly pb = poly_[b];
        const bool integer = cm::is_integer(ev);
        if (pb.len == 1) {
          const Mono m = mpool_[pb.begin];
          // (c * x^a * y^b)^e = c^e x^(ae) y^(be) for integer e; a lone factor with c == 1 also works
          // for fractional e when its exponent is 1, since the power is then taken of the atom itself.
          if (integer || (m.coef == 1.0 && m.len == 1 && fpool_[m.begin].second == 1.0)) {
            const auto fb = static_cast<std::uint32_t>(fpool_.size());
            for (std::uint32_t k = 0; k < m.len; ++k) {
              const auto f = fpool_[m.begin + k];
              fpool_.emplace_back(f.first, f.second * ev);
            }
            const auto mb = static_cast<std::uint32_t>(mpool_.size());
            mpool_.push_back(Mono{cm::pow(m.coef, ev), fb, m.len});
            return Poly{mb, 1};
          }
        }
        if (integer && ev > 1 && ev <= 4) {
          Poly acc = pb;
          bool ok = true;
          for (int p = 1; p < static_cast<int>(ev) && ok; ++p) {
            ok = static_cast<std::size_t>(acc.len) * pb.len <= max_terms_;
            if (ok) acc = product(acc, pb);
          }
          if (ok) return acc;
        }
        return atom(pb.len == 1 ? b : materialize(b), ev);  // power of the (factored) base
      }
      case Op::Var:
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
      case Op::Atan2:
      case Op::Min:
      case Op::Max:
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
      case Op::Where: return atom(i, 1.0);
    }
    return atom(i, 1.0);
  }
};

}  // namespace detail

// Expands each root into a polynomial over atoms (function applications, variables, oversized
// subexpressions) and re-factors it with multivariate Horner. Intermediate results are never interned.
constexpr Buffer<id_t> expand_and_factor(Graph& g, const Buffer<id_t>& roots, std::size_t max_terms = 32) {
  return detail::PolyExpander(g, max_terms).run(roots);
}

// Factors every sum in the arithmetic region of `roots` in place (multivariate Horner over its
// existing terms, no expansion). Cheap enough to run by default.
constexpr Buffer<id_t> factor_sums(Graph& g, const Buffer<id_t>& roots) {
  const std::size_t n0 = g.size();
  const Buffer<char> live = detail::polynomial_region(g, roots);
  Buffer<id_t> m(n0, kNone);
  Buffer<id_t> a;
  Buffer<std::pair<id_t, double>> fs;
  detail::Horner horner(g);
  for (id_t i = 0; i < n0; ++i) {
    if (!live[i]) continue;
    const Node n = g.node(i);
    if (n.op != Op::Add && n.op != Op::Mul && n.op != Op::Pow) {
      m[i] = i;
      continue;
    }
    a.clear();
    bool changed = false;
    for (std::uint32_t k = 0; k < n.count; ++k) {
      const id_t c = g.arg(i, k);
      a.push_back(m[c]);
      changed |= m[c] != c;
    }
    const id_t x = changed ? g.rebuild(i, a) : i;
    if (g.op(x) != Op::Add || g.nargs(x) < 2) {
      m[i] = x;
      continue;
    }
    horner.clear();
    for (std::uint32_t k = 0; k < g.nargs(x); ++k) {
      const id_t t = g.arg(x, k);
      fs.clear();
      double coef = 1.0;
      auto push = [&](id_t f) {
        if (g.op(f) == Op::Pow && g.is_const(g.arg(f, 1))) fs.emplace_back(g.arg(f, 0), g.value(g.arg(f, 1)));
        else fs.emplace_back(f, 1.0);
      };
      if (g.op(t) == Op::Mul) {
        coef = g.value(t);
        for (std::uint32_t j = 0; j < g.nargs(t); ++j) push(g.arg(t, j));
      } else {
        push(t);
      }
      horner.add_term(coef, fs.data(), static_cast<std::uint32_t>(fs.size()));
    }
    m[i] = g.add(horner.result(), g.constant(g.value(x)));
  }
  Buffer<id_t> out;
  out.reserve(roots.size());
  for (const id_t r : roots) out.push_back(m[r]);
  return out;
}

// Per root, the cheaper of `roots[r]` and `alt[r]` by marginal op count (shared work is free).
// `keep` roots are costed first and never replaced.
constexpr Buffer<id_t> select_cheaper(Graph& g, const Buffer<id_t>& keep, const Buffer<id_t>& roots,
                                      const Buffer<id_t>& alt) {
  detail::CostModel cost(g);
  for (const id_t k : keep) cost.marginal(k, true);
  Buffer<id_t> out;
  out.reserve(roots.size());
  for (std::size_t r = 0; r < roots.size(); ++r) {
    const std::size_t c0 = cost.marginal(roots[r], false);
    const std::size_t c1 = alt[r] == roots[r] ? c0 : cost.marginal(alt[r], false);
    const id_t pick = c1 < c0 ? alt[r] : roots[r];
    cost.marginal(pick, true);
    out.push_back(pick);
  }
  return out;
}

// Per-root choice between the given form and its expanded+factored form, by marginal op count.
// `keep` roots (e.g. residual outputs) are costed first and never rewritten.
constexpr Buffer<id_t> optimize(Graph& g, const Buffer<id_t>& keep, const Buffer<id_t>& roots) {
  return select_cheaper(g, keep, roots, expand_and_factor(g, roots));
}

}  // namespace csym
