#pragma once

// Lowering: canonical n-ary DAG -> flat tape of binary/unary instructions.
//
//  * Add terms with negative coefficients become subtractions; Mul factors with negative exponents
//    become one division; integer powers become multiplication chains (binary powering); half-integer
//    powers use sqrt.
//  * A second level of value numbering (hash-consing on (op, a, b, c)) shares identical instructions
//    that lowering produces from different canonical nodes, e.g. x*x or 1/sqrt(x).
//  * Dead-code elimination runs at the end, since lowering visits every reachable canonical node,
//    including some whose standalone form is never used (e.g. a Pow(x,-1) that only feeds a division).

#include <bit>
#include <cstdint>

#include "csym/core/buffer.h"
#include "csym/core/graph.h"

namespace csym {

enum class IOp : std::uint8_t {
  In,     // in[a]
  Const,  // consts[a]
  Add,
  Sub,
  Mul,
  Div,
  Neg,
  Sqrt,
  Pow,
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
  Atan2,
  Min,
  Max,
  Lt,
  Le,
  Eq,
  Where,  // a ? b : c
};

struct Instr {
  IOp op;
  std::uint32_t a, b, c;
};

constexpr bool is_arithmetic_op(IOp op) { return op != IOp::In && op != IOp::Const; }

// Number of floating-point operations the tape performs (inputs and constants are free).
constexpr std::size_t count_ops(const Instr* instrs, std::size_t n) {
  std::size_t k = 0;
  for (std::size_t i = 0; i < n; ++i) k += is_arithmetic_op(instrs[i].op);
  return k;
}

// A run of instructions needed by the same set of output groups (bit g = group g).
struct Segment {
  std::uint32_t begin, end, mask;
};

struct LoweredProgram {
  Buffer<Instr> instrs;
  Buffer<double> consts;
  Buffer<std::uint32_t> outputs;
  Buffer<std::uint32_t> output_groups;  // per output: group index (< 32)
  Buffer<Segment> segments;             // partition of instrs; see Lowerer::segment()
  std::uint32_t num_inputs = 0;
};

namespace detail {

constexpr IOp to_iop(Op op) {
  switch (op) {
    case Op::Sin: return IOp::Sin;
    case Op::Cos: return IOp::Cos;
    case Op::Tan: return IOp::Tan;
    case Op::Asin: return IOp::Asin;
    case Op::Acos: return IOp::Acos;
    case Op::Atan: return IOp::Atan;
    case Op::Exp: return IOp::Exp;
    case Op::Log: return IOp::Log;
    case Op::Tanh: return IOp::Tanh;
    case Op::Abs: return IOp::Abs;
    case Op::Sign: return IOp::Sign;
    case Op::SignNoZero: return IOp::SignNoZero;
    case Op::Floor: return IOp::Floor;
    case Op::Atan2: return IOp::Atan2;
    case Op::Min: return IOp::Min;
    case Op::Max: return IOp::Max;
    case Op::Lt: return IOp::Lt;
    case Op::Le: return IOp::Le;
    case Op::Eq: return IOp::Eq;
    case Op::Where: return IOp::Where;
    // The n-ary nodes lower through lower_node(), never through here; the
    // nearest instruction is what a caller that got here by mistake would see.
    case Op::Const: return IOp::Const;
    case Op::Var: return IOp::In;
    case Op::Add: return IOp::Add;
    case Op::Mul: return IOp::Mul;
    case Op::Pow: return IOp::Pow;
  }
  return IOp::Where;
}

class Lowerer {
 public:
  constexpr Lowerer(const Graph& g, std::uint32_t num_inputs) : g_(g), table_(1024, 0) {
    p_.num_inputs = num_inputs;
  }

  constexpr LoweredProgram run(const Buffer<id_t>& roots, const Buffer<std::uint32_t>& groups) {
    const Buffer<char> live = g_.reachable(roots);
    memo_ = Buffer<std::uint32_t>(g_.size(), kNone);
    plan_products(live);
    for (int round = 0; round < 3; ++round)
      if (!share_pairs()) break;
    count_denominators();
    for (id_t i = 0; i < g_.size(); ++i)
      if (live[i]) memo_[i] = lower_node(i);
    for (const id_t r : roots) p_.outputs.push_back(memo_[r]);
    eliminate_dead_code();
    p_.output_groups = groups.empty() ? Buffer<std::uint32_t>(roots.size(), 0) : groups;
    segment();
    return std::move(p_);
  }

 private:
  const Graph& g_;
  LoweredProgram p_;
  Buffer<std::uint32_t> table_;
  Buffer<std::uint32_t> memo_;

  // ---- product planning and pair sharing ------------------------------------------------------
  // Every Mul node is planned as a list of elements; an element is an atom, possibly inverted
  // (a denominator factor). Atoms are a base raised to a positive numeric power, an opaque factor, or
  // the product / quotient of two other atoms. Pairs of elements that occur together in several
  // products are hoisted into shared atoms (greedy, most frequent first), so e.g. sin(h)^2 / n^2 is
  // computed once rather than inside every product that uses it. Flattened n-ary products otherwise
  // lose such sub-products.
  //
  // Element encoding: atom index * 2 + (1 if inverted).
  enum class AtomKind : std::uint8_t { Power, Product, Quotient };
  struct Atom {
    AtomKind kind;
    id_t base;           // Power: graph node
    double exponent;     // Power: positive
    std::uint32_t a, b;  // Product / Quotient: atom indices
    std::uint32_t instr;
  };
  struct Plan {
    std::uint32_t begin, len;
  };
  Buffer<Atom> atoms_;
  Buffer<std::uint32_t> atom_table_;  // open addressing over (base, exponent) -> atom index + 1
  Buffer<Plan> plan_;                 // per graph node (Mul only)
  Buffer<std::uint32_t> lists_;       // element lists referenced by plans
  Buffer<std::uint32_t> list_owner_;  // list start offset -> owning node
  Buffer<std::uint32_t> inv_uses_;    // per atom: number of products dividing by it
  Buffer<std::uint32_t> recip_;       // per atom: instruction computing 1/atom, once emitted

  // A denominator shared by several products is inverted once and multiplied in everywhere else:
  // one division plus multiplications instead of a division per product.
  constexpr void count_denominators() {
    inv_uses_ = Buffer<std::uint32_t>(atoms_.size(), 0);
    recip_ = Buffer<std::uint32_t>(atoms_.size(), kNone);
    Buffer<char> used(atoms_.size(), 0);
    for (id_t m = 0; m < g_.size(); ++m) {
      const Plan p = plan_[m];
      for (std::uint32_t k = 0; k < p.len; ++k) {
        const std::uint32_t e = lists_[p.begin + k];
        used[e >> 1] = 1;
        if (e & 1) ++inv_uses_[e >> 1];
      }
    }
    // Quotient atoms still in use divide by their second operand too. Pair atoms only reference
    // older atoms, so one reverse sweep propagates usage.
    for (std::size_t i = atoms_.size(); i-- > 0;) {
      if (!used[i] || atoms_[i].kind == AtomKind::Power) continue;
      used[atoms_[i].a] = used[atoms_[i].b] = 1;
      if (atoms_[i].kind == AtomKind::Quotient) ++inv_uses_[atoms_[i].b];
    }
  }
  constexpr std::uint32_t reciprocal(std::uint32_t atom) {
    if (recip_[atom] == kNone) {
      const std::uint32_t x = atom_instr(atom);
      recip_[atom] = emit(IOp::Div, constant(1.0), x);
    }
    return recip_[atom];
  }

  constexpr std::uint32_t atom_of(id_t base, double exponent) {
    if (atom_table_.empty()) atom_table_ = Buffer<std::uint32_t>(1024, 0);
    if ((atoms_.size() + 1) * 2 > atom_table_.size()) {
      Buffer<std::uint32_t> t(atom_table_.size() * 2, 0);
      const std::size_t mask = t.size() - 1;
      for (std::size_t i = 0; i < atoms_.size(); ++i) {
        if (atoms_[i].kind != AtomKind::Power) continue;
        std::size_t slot = atom_hash(atoms_[i].base, atoms_[i].exponent) & mask;
        while (t[slot]) slot = (slot + 1) & mask;
        t[slot] = static_cast<std::uint32_t>(i + 1);
      }
      atom_table_ = std::move(t);
    }
    const std::size_t mask = atom_table_.size() - 1;
    for (std::size_t slot = atom_hash(base, exponent) & mask;; slot = (slot + 1) & mask) {
      const std::uint32_t e = atom_table_[slot];
      if (e == 0) {
        atoms_.push_back(Atom{AtomKind::Power, base, exponent, 0, 0, kNone});
        atom_table_[slot] = static_cast<std::uint32_t>(atoms_.size());
        return static_cast<std::uint32_t>(atoms_.size() - 1);
      }
      const Atom& at = atoms_[e - 1];
      if (at.kind == AtomKind::Power && at.base == base && at.exponent == exponent) return e - 1;
    }
  }
  static constexpr std::uint64_t atom_hash(id_t base, double e) {
    std::uint64_t h = (static_cast<std::uint64_t>(base) + 1) * 0x9e3779b97f4a7c15ull;
    h ^= std::bit_cast<std::uint64_t>(e) * 0xbf58476d1ce4e5b9ull;
    return h ^ (h >> 31);
  }

  constexpr void plan_products(const Buffer<char>& live) {
    plan_ = Buffer<Plan>(g_.size(), Plan{0, 0});
    list_owner_.clear();
    for (id_t m = 0; m < g_.size(); ++m) {
      if (!live[m] || g_.op(m) != Op::Mul) continue;
      const auto begin = static_cast<std::uint32_t>(lists_.size());
      for (std::uint32_t k = 0; k < g_.nargs(m); ++k) {
        const id_t f = g_.arg(m, k);
        if (g_.op(f) == Op::Pow && g_.is_const(g_.arg(f, 1))) {
          const double ev = g_.value(g_.arg(f, 1));
          lists_.push_back(atom_of(g_.arg(f, 0), cm::fabs(ev)) * 2 + (ev < 0 ? 1 : 0));
        } else {
          lists_.push_back(atom_of(f, 1.0) * 2);
        }
      }
      plan_[m] = Plan{begin, static_cast<std::uint32_t>(lists_.size()) - begin};
      while (list_owner_.size() < lists_.size()) list_owner_.push_back(kNone);
      if (plan_[m].len) list_owner_[begin] = m;
    }
  }

  // Shared atom for a pair of elements; returns the element that replaces them.
  constexpr std::uint32_t pair_element(std::uint32_t x, std::uint32_t y) {
    const bool ix = x & 1, iy = y & 1;
    if (ix && !iy) std::swap(x, y);  // numerator first
    const bool inv_x = x & 1, inv_y = y & 1;
    if (!inv_x && inv_y) {
      atoms_.push_back(Atom{AtomKind::Quotient, kNone, 1.0, x >> 1, y >> 1, kNone});
      return static_cast<std::uint32_t>(atoms_.size() - 1) * 2;
    }
    atoms_.push_back(Atom{AtomKind::Product, kNone, 1.0, x >> 1, y >> 1, kNone});
    return static_cast<std::uint32_t>(atoms_.size() - 1) * 2 + (inv_x ? 1 : 0);  // both inverted: 1/(x*y)
  }

  // One round of pair sharing over all planned lists. Returns whether anything was shared.
  constexpr bool share_pairs() {
    struct Occ {
      std::uint32_t a, b, list;
    };
    Buffer<Occ> occ;
    for (id_t m = 0; m < g_.size(); ++m) {
      const Plan p = plan_[m];
      if (p.len < 2) continue;
      const std::size_t first = occ.size();
      for (std::uint32_t i = 0; i < p.len; ++i)
        for (std::uint32_t j = i + 1; j < p.len; ++j) {
          std::uint32_t a = lists_[p.begin + i], b = lists_[p.begin + j];
          if (b < a) std::swap(a, b);
          bool dup = false;
          for (std::size_t k = first; k < occ.size() && !dup; ++k) dup = occ[k].a == a && occ[k].b == b;
          if (!dup) occ.push_back(Occ{a, b, p.begin});
        }
    }
    if (occ.empty()) return false;
    sort(occ, [](const Occ& x, const Occ& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    struct Cand {
      std::uint32_t begin, count;  // run in occ
    };
    Buffer<Cand> cands;
    for (std::uint32_t i = 0; i < occ.size();) {
      std::uint32_t j = i + 1;
      while (j < occ.size() && occ[j].a == occ[i].a && occ[j].b == occ[i].b) ++j;
      if (j - i >= 2) cands.push_back(Cand{i, j - i});
      i = j;
    }
    sort(cands, [](const Cand& x, const Cand& y) { return x.count > y.count; });
    bool shared = false;
    for (const Cand& c : cands) {
      const std::uint32_t a = occ[c.begin].a, b = occ[c.begin].b;
      // Apply to every list that still holds both elements (it may have been rewritten since).
      std::uint32_t hits = 0, pair = kNone;
      for (std::uint32_t k = c.begin; k < c.begin + c.count; ++k) {
        Plan& plan = plan_[list_owner_[occ[k].list]];
        std::uint32_t ia = kNone, ib = kNone;
        for (std::uint32_t i = 0; i < plan.len; ++i) {
          const std::uint32_t e = lists_[plan.begin + i];
          if (ia == kNone && e == a) ia = i;
          else if (ib == kNone && e == b) ib = i;
        }
        if (ia == kNone || ib == kNone) continue;
        if (pair == kNone) pair = pair_element(a, b);
        lists_[plan.begin + ia] = pair;
        for (std::uint32_t i = ib; i + 1 < plan.len; ++i) lists_[plan.begin + i] = lists_[plan.begin + i + 1];
        --plan.len;
        ++hits;
      }
      shared |= hits >= 2;
    }
    return shared;
  }

  constexpr std::uint32_t atom_instr(std::uint32_t i) {
    if (atoms_[i].instr != kNone) return atoms_[i].instr;
    const Atom at = atoms_[i];
    std::uint32_t r;
    if (at.kind == AtomKind::Power) {
      r = power(memo_[at.base], at.exponent);
    } else {
      const std::uint32_t x = atom_instr(at.a);
      if (at.kind == AtomKind::Product) r = emit(IOp::Mul, x, atom_instr(at.b));
      else if (at.b < inv_uses_.size() && inv_uses_[at.b] >= 2) r = emit(IOp::Mul, x, reciprocal(at.b));
      else r = emit(IOp::Div, x, atom_instr(at.b));
    }
    atoms_[i].instr = r;
    return r;
  }

  constexpr std::uint32_t lower_node(id_t i) {
    const Node n = g_.node(i);
    switch (n.op) {
      case Op::Const: return constant(n.value);
      case Op::Var: return emit(IOp::In, g_.var_index(i));
      case Op::Add: return lower_add(i);
      case Op::Mul: {
        const std::uint32_t r = lower_product(cm::fabs(n.value), i);
        return n.value < 0 ? emit(IOp::Neg, r) : r;
      }
      case Op::Pow: {
        const id_t b = g_.arg(i, 0), e = g_.arg(i, 1);
        if (g_.is_const(e)) {
          const double ev = g_.value(e);
          if (ev > 0) return power(memo_[b], ev);
          return emit(IOp::Div, constant(1.0), power(memo_[b], -ev));
        }
        return emit(IOp::Pow, memo_[b], memo_[e]);
      }
      case Op::Where: return emit(IOp::Where, memo_[g_.arg(i, 0)], memo_[g_.arg(i, 1)], memo_[g_.arg(i, 2)]);
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
        return emit(to_iop(n.op), memo_[g_.arg(i, 0)]);
      case Op::Atan2:
      case Op::Min:
      case Op::Max:
      case Op::Lt:
      case Op::Le:
      case Op::Eq:
        return emit(to_iop(n.op), memo_[g_.arg(i, 0)], memo_[g_.arg(i, 1)]);
    }
    return kNone;  // unreachable: every Op is handled above
  }

  // |coef| * factors of Mul node `m` (coef is passed separately so Add can fold the sign into a Sub).
  constexpr std::uint32_t lower_product(double mag, id_t m) {
    Buffer<std::uint32_t> num, den;
    const Plan p = plan_[m];
    for (std::uint32_t k = 0; k < p.len; ++k) {
      const std::uint32_t e = lists_[p.begin + k], atom = e >> 1;
      if (!(e & 1)) num.push_back(atom_instr(atom));
      else if (atom < inv_uses_.size() && inv_uses_[atom] >= 2) num.push_back(reciprocal(atom));
      else den.push_back(atom_instr(atom));
    }
    std::uint32_t top;
    if (num.empty()) top = constant(mag);
    else if (mag == 1.0) top = product(num);
    else top = emit(IOp::Mul, constant(mag), product(num));
    if (den.empty()) return top;
    return emit(IOp::Div, top, product(den));
  }

  // Terms sharing a coefficient magnitude are summed first and scaled once: 2a + 2b - 2c -> 2(a + b - c).
  constexpr std::uint32_t lower_add(id_t a) {
    struct Group {
      double mag;
      Buffer<std::uint32_t> pos, neg;
    };
    Buffer<Group> groups;
    for (std::uint32_t k = 0; k < g_.nargs(a); ++k) {
      const id_t t = g_.arg(a, k);
      double coef = 1.0;
      std::uint32_t x;
      if (g_.op(t) == Op::Mul) {
        coef = g_.value(t);
        x = lower_product(1.0, t);
      } else {
        x = memo_[t];
      }
      const double mag = cm::fabs(coef);
      std::size_t gi = 0;
      while (gi < groups.size() && groups[gi].mag != mag) ++gi;
      if (gi == groups.size()) groups.push_back(Group{mag, {}, {}});
      (coef < 0 ? groups[gi].neg : groups[gi].pos).push_back(x);
    }
    Buffer<std::uint32_t> pos, neg;
    for (const auto& grp : groups) {
      const bool negative = grp.pos.empty();
      std::uint32_t s = kNone;
      for (const auto p : grp.pos) s = s == kNone ? p : emit(IOp::Add, s, p);
      for (const auto q : grp.neg) s = s == kNone ? q : emit(negative ? IOp::Add : IOp::Sub, s, q);
      if (grp.mag != 1.0) s = emit(IOp::Mul, constant(grp.mag), s);
      (negative ? neg : pos).push_back(s);
    }
    std::uint32_t acc = kNone;
    for (const auto p : pos) acc = acc == kNone ? p : emit(IOp::Add, acc, p);
    const double c = g_.value(a);
    if (c != 0.0) {
      if (acc == kNone) acc = constant(c);
      else if (c > 0) acc = emit(IOp::Add, acc, constant(c));
      else acc = emit(IOp::Sub, acc, constant(-c));
    }
    for (const auto q : neg) acc = acc == kNone ? emit(IOp::Neg, q) : emit(IOp::Sub, acc, q);
    return acc;
  }

  constexpr std::uint32_t product(const Buffer<std::uint32_t>& xs) {
    std::uint32_t acc = xs[0];
    for (std::size_t k = 1; k < xs.size(); ++k) acc = emit(IOp::Mul, acc, xs[k]);
    return acc;
  }

  // x^e for e > 0.
  constexpr std::uint32_t power(std::uint32_t x, double e) {
    if (cm::is_integer(e) && e < 1e6) {
      auto n = static_cast<std::uint64_t>(e);
      std::uint32_t acc = kNone, base = x;
      while (n) {
        if (n & 1) acc = acc == kNone ? base : emit(IOp::Mul, acc, base);
        n >>= 1;
        if (n) base = emit(IOp::Mul, base, base);
      }
      return acc;
    }
    if (cm::is_integer(2.0 * e)) {
      const std::uint32_t s = emit(IOp::Sqrt, x);
      return e < 1.0 ? s : emit(IOp::Mul, power(x, e - 0.5), s);
    }
    return emit(IOp::Pow, x, constant(e));
  }

  constexpr std::uint32_t constant(double v) {
    const auto bits = std::bit_cast<std::uint64_t>(v);
    std::uint32_t idx = 0;
    for (; idx < p_.consts.size(); ++idx)
      if (std::bit_cast<std::uint64_t>(p_.consts[idx]) == bits) break;
    if (idx == p_.consts.size()) p_.consts.push_back(v);
    return emit(IOp::Const, idx);
  }

  static constexpr bool commutative(IOp op) {
    return op == IOp::Add || op == IOp::Mul || op == IOp::Min || op == IOp::Max || op == IOp::Eq;
  }
  static constexpr std::uint64_t hash(const Instr& k) {
    std::uint64_t h = static_cast<std::uint64_t>(k.op) * 0x9e3779b97f4a7c15ull;
    h = (h ^ k.a) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ k.b) * 0x94d049bb133111ebull;
    h = (h ^ k.c) * 0xbf58476d1ce4e5b9ull;
    return h ^ (h >> 29);
  }

  constexpr std::uint32_t emit(IOp op, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0) {
    if (commutative(op) && b < a) std::swap(a, b);
    const Instr k{op, a, b, c};
    if ((p_.instrs.size() + 1) * 2 > table_.size()) grow();
    const std::size_t mask = table_.size() - 1;
    for (std::size_t slot = hash(k) & mask;; slot = (slot + 1) & mask) {
      const std::uint32_t e = table_[slot];
      if (e == 0) {
        p_.instrs.push_back(k);
        table_[slot] = static_cast<std::uint32_t>(p_.instrs.size());
        return static_cast<std::uint32_t>(p_.instrs.size() - 1);
      }
      const Instr& o = p_.instrs[e - 1];
      if (o.op == k.op && o.a == k.a && o.b == k.b && o.c == k.c) return e - 1;
    }
  }
  constexpr void grow() {
    Buffer<std::uint32_t> t(table_.size() * 2, 0);
    const std::size_t mask = t.size() - 1;
    for (std::size_t i = 0; i < p_.instrs.size(); ++i) {
      std::size_t slot = hash(p_.instrs[i]) & mask;
      while (t[slot]) slot = (slot + 1) & mask;
      t[slot] = static_cast<std::uint32_t>(i + 1);
    }
    table_ = std::move(t);
  }

  static constexpr int arity(IOp op) {
    switch (op) {
      case IOp::In:
      case IOp::Const: return 0;
      case IOp::Add:
      case IOp::Sub:
      case IOp::Mul:
      case IOp::Div:
      case IOp::Pow:
      case IOp::Atan2:
      case IOp::Min:
      case IOp::Max:
      case IOp::Lt:
      case IOp::Le:
      case IOp::Eq: return 2;
      case IOp::Where: return 3;
      case IOp::Neg:
      case IOp::Sqrt:
      case IOp::Sin:
      case IOp::Cos:
      case IOp::Tan:
      case IOp::Asin:
      case IOp::Acos:
      case IOp::Atan:
      case IOp::Exp:
      case IOp::Log:
      case IOp::Tanh:
      case IOp::Abs:
      case IOp::Sign:
      case IOp::SignNoZero:
      case IOp::Floor: return 1;
    }
    return 1;
  }

  // Orders instructions so each output group's work is contiguous where possible, and splits the tape
  // into segments by the set of groups that need each instruction. The evaluator can then skip every
  // segment no requested group needs (e.g. Jacobian blocks the caller did not ask for).
  //
  // An operand is needed by at least the groups its user is needed by, so its mask is a superset and
  // has at least as many bits; a stable sort by descending popcount therefore keeps a valid
  // topological order (equal popcount and superset implies equal masks, whose order is preserved).
  constexpr void segment() {
    const std::size_t n = p_.instrs.size();
    Buffer<std::uint32_t> mask(n, 0);
    for (std::size_t j = 0; j < p_.outputs.size(); ++j) mask[p_.outputs[j]] |= 1u << p_.output_groups[j];
    for (std::size_t i = n; i-- > 0;) {
      const Instr& k = p_.instrs[i];
      const int ar = arity(k.op);
      if (ar >= 1) mask[k.a] |= mask[i];
      if (ar >= 2) mask[k.b] |= mask[i];
      if (ar >= 3) mask[k.c] |= mask[i];
    }
    Buffer<std::uint32_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = static_cast<std::uint32_t>(i);
    auto group_before = [&](std::uint32_t x, std::uint32_t y) {
      const int px = std::popcount(mask[x]), py = std::popcount(mask[y]);
      return px != py ? px > py : mask[x] < mask[y];
    };
    sort(order, group_before);
    Buffer<std::uint32_t> remap(n);
    for (std::size_t i = 0; i < n; ++i) remap[order[i]] = static_cast<std::uint32_t>(i);
    Buffer<Instr> out(n);
    for (std::size_t i = 0; i < n; ++i) {
      Instr k = p_.instrs[order[i]];
      const int ar = arity(k.op);
      if (ar >= 1) k.a = remap[k.a];
      if (ar >= 2) k.b = remap[k.b];
      if (ar >= 3) k.c = remap[k.c];
      out[i] = k;
    }
    for (auto& o : p_.outputs) o = remap[o];
    p_.instrs = std::move(out);
    p_.segments.clear();
    for (std::size_t i = 0; i < n;) {
      const std::uint32_t m = mask[order[i]];
      std::size_t j = i + 1;
      while (j < n && mask[order[j]] == m) ++j;
      p_.segments.push_back(Segment{static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j), m});
      i = j;
    }
  }

  constexpr void eliminate_dead_code() {
    const std::size_t n = p_.instrs.size();
    Buffer<char> live(n, 0);
    for (const auto o : p_.outputs) live[o] = 1;
    for (std::size_t i = n; i-- > 0;) {
      if (!live[i]) continue;
      const Instr& k = p_.instrs[i];
      const int ar = arity(k.op);
      if (ar >= 1) live[k.a] = 1;
      if (ar >= 2) live[k.b] = 1;
      if (ar >= 3) live[k.c] = 1;
    }
    Buffer<std::uint32_t> remap(n, kNone);
    Buffer<Instr> out;
    Buffer<char> const_used(p_.consts.size(), 0);
    for (std::size_t i = 0; i < n; ++i) {
      if (!live[i]) continue;
      Instr k = p_.instrs[i];
      const int ar = arity(k.op);
      if (ar >= 1) k.a = remap[k.a];
      if (ar >= 2) k.b = remap[k.b];
      if (ar >= 3) k.c = remap[k.c];
      if (k.op == IOp::Const) const_used[k.a] = 1;
      remap[i] = static_cast<std::uint32_t>(out.size());
      out.push_back(k);
    }
    // Compact the constant pool too.
    Buffer<std::uint32_t> cremap(p_.consts.size(), kNone);
    Buffer<double> consts;
    for (std::size_t c = 0; c < p_.consts.size(); ++c) {
      if (!const_used[c]) continue;
      cremap[c] = static_cast<std::uint32_t>(consts.size());
      consts.push_back(p_.consts[c]);
    }
    for (auto& k : out)
      if (k.op == IOp::Const) k.a = cremap[k.a];
    for (auto& o : p_.outputs) o = remap[o];
    p_.instrs = std::move(out);
    p_.consts = std::move(consts);
  }
};

}  // namespace detail

// `groups` (optional) assigns each root an output group index < 32; default all 0.
constexpr LoweredProgram lower(const Graph& g, const Buffer<id_t>& roots, std::uint32_t num_inputs,
                               const Buffer<std::uint32_t>& groups = {}) {
  return detail::Lowerer(g, num_inputs).run(roots, groups);
}

}  // namespace csym
