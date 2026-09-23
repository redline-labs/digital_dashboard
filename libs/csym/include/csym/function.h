#pragma once

// csym::Function: compile a generic C++ function into straight-line numeric code, at compile time.
//
//   constexpr auto f = [](auto x, auto y) { return x * sin(y); };
//   using Fn = csym::Function<f, double, double>;
//   double v = Fn::eval(1.0, 0.3);
//   auto [value, J] = Fn::jacobian(1.0, 0.3);           // J is 1 x 2
//   auto [value, Jy] = Fn::jacobian<1>(1.0, 0.3);       // only with respect to argument 1
//   auto lin = Fn::linearize(1.0, 0.3);                 // residual, jacobian, hessian = JᵀJ, rhs = Jᵀr
//
//   Fn::Block<0> H0;                                     // GTSAM style: one block per argument,
//   double v = Fn::evaluate<0, 1>(1.0, 0.3, &H0, nullptr);  // null blocks are skipped at runtime
//
// Argument types are numeric (double, float, Matrix<double,...>, Rot3<double>, ...). Internally the
// function is traced with their symbolic (Expr) counterparts in a consteval context, differentiated,
// simplified, lowered to a fixed-size instruction tape, and evaluated by fully unrolled code.
//
// Jacobians are taken with respect to each argument's tangent space (see lie.h) and stacked in
// argument order, like SymForce's linearization. When the output is a Lie group (or contains one),
// Jacobian rows are in the output's tangent space too, as in SymForce; the returned value is still the
// output itself.
//
// All of eval/jacobian/evaluate/linearize are constexpr: they also work inside static_assert.

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "csym/config.h"
#include "csym/core/batch.h"
#include "csym/core/buffer.h"
#include "csym/core/expr.h"
#include "csym/core/simplify.h"
#include "csym/core/storage.h"
#include "csym/lie.h"
#include "csym/lower.h"
#include "csym/matrix.h"

namespace csym {

enum class Mode { Value, Jacobian, Linearization };

template <std::size_t... I>
struct Wrt {};

template <class Out, class S, std::size_t M, std::size_t N>
struct WithJacobian {
  Out value;
  Matrix<S, M, N> jacobian;
};

template <class S, std::size_t M, std::size_t N>
struct Linearization {
  Matrix<S, M, 1> residual;
  Matrix<S, M, N> jacobian;
  Matrix<S, N, N> hessian;  // JᵀJ: lower triangle (upper triangle is zero), as in SymForce
  Matrix<S, N, 1> rhs;      // Jᵀr
};

template <std::size_t NI, std::size_t NC, std::size_t NO, std::size_t NS>
struct Tape {
  std::array<Instr, NI> instrs;
  std::array<double, NC> consts;
  std::array<std::uint32_t, NO> outputs;
  std::array<std::uint32_t, NO> output_groups;
  std::array<Segment, NS> segments;
  std::uint32_t num_inputs;
  static constexpr std::size_t num_instrs = NI, num_consts = NC, num_outputs = NO, num_segments = NS;
  constexpr std::size_t op_count() const { return count_ops(instrs.data(), NI); }
};

// Opt-in extra simplification: Function<csym::optimized<f>, ...> also tries an expanded and
// re-factored form of every output (see core/simplify.h) and keeps whichever is cheaper. It can
// shave some operations off Jacobians of polynomial-heavy functions, at a large compile-time cost
// (roughly 10x on the geometry benchmarks), so it is off by default.
template <auto F>
struct Optimized {
  template <class... A>
  constexpr auto operator()(const A&... a) const {
    return F(a...);
  }
};
template <auto F>
inline constexpr Optimized<F> optimized{};

template <class T>
inline constexpr bool is_optimized = false;
template <auto F>
inline constexpr bool is_optimized<Optimized<F>> = true;

// ---- argument presentation ------------------------------------------------------------------------------
// An argument type may differ from what the residual function receives: e.g. GtsamPose2 (GTSAM tangent
// conventions for its Jacobian columns) is presented to the function as a plain Pose2.
template <class A>
CSYM_ALWAYS_INLINE constexpr const auto& present(const A& a) {
  if constexpr (requires { typename A::presented_as; }) return static_cast<const typename A::presented_as&>(a);
  else return a;
}

// ---- output tangent space ------------------------------------------------------------------------
// Maps Jacobian rows from an output's storage to its tangent space (identity for vector spaces).
template <class O>
struct output_tangent {
  static constexpr std::size_t dim = lie<O>::tangent_dim;
  static constexpr void map(Graph& g, const O& out, const id_t* s, id_t* t) {
    if constexpr (lie<O>::identity_tangent) {
      for (std::size_t i = 0; i < storage_dim<O>; ++i) t[i] = s[i];
    } else {
      const auto M = lie<O>::tangent_D_storage(out);
      for (std::size_t i = 0; i < dim; ++i) {
        Buffer<id_t> terms;
        for (std::size_t k = 0; k < storage_dim<O>; ++k) {
          const id_t m = M(i, k).node(g);
          if (s[k] != g.zero && !g.is_const(m, 0.0)) terms.push_back(g.mul(s[k], m));
        }
        t[i] = g.add(terms);
      }
    }
  }
};
template <class... Ts>
struct output_tangent<std::tuple<Ts...>> {
  static constexpr std::size_t dim = (output_tangent<Ts>::dim + ... + 0);
  static constexpr void map(Graph& g, const std::tuple<Ts...>& out, const id_t* s, id_t* t) {
    std::apply([&](const auto&... e) {
      ((output_tangent<std::remove_cvref_t<decltype(e)>>::map(g, e, s, t),
        s += storage_dim<decltype(e)>, t += output_tangent<std::remove_cvref_t<decltype(e)>>::dim),
       ...);
    }, out);
  }
};
template <class T, std::size_t N>
struct output_tangent<std::array<T, N>> {
  static constexpr std::size_t dim = N * output_tangent<T>::dim;
  static constexpr void map(Graph& g, const std::array<T, N>& out, const id_t* s, id_t* t) {
    for (std::size_t i = 0; i < N; ++i)
      output_tangent<T>::map(g, out[i], s + i * storage_dim<T>, t + i * output_tangent<T>::dim);
  }
};

namespace detail {

template <std::size_t, class S>
using pointer_for = S*;

struct TapeSizes {
  std::size_t instrs, consts, outputs, segments;
};

template <std::size_t... I>
constexpr Wrt<I...> all_wrt(std::index_sequence<I...>) {
  return {};
}

template <class Tuple, std::size_t... I>
constexpr auto arg_offsets(std::index_sequence<I...>) {
  std::array<std::size_t, sizeof...(I) + 1> o{};
  std::size_t acc = 0;
  ((o[I] = acc, acc += storage_dim<std::tuple_element_t<I, Tuple>>), ...);
  o[sizeof...(I)] = acc;
  return o;
}

// Appends the tangent-space Jacobian columns of `roots` with respect to argument I: one directional
// derivative per tangent direction, seeded with the corresponding column of storage_D_tangent.
template <std::size_t I, class SymArgs>
constexpr void append_tangent_columns(Graph& g, const SymArgs& args, const Buffer<Expr>& inputs,
                                      std::size_t offset, const Buffer<id_t>& roots,
                                      Buffer<Buffer<id_t>>& cols) {
  using A = std::tuple_element_t<I, SymArgs>;
  constexpr std::size_t sdim = storage_dim<A>;
  if constexpr (lie<A>::identity_tangent) {
    for (std::size_t k = 0; k < sdim; ++k) cols.push_back(g.diff(roots, inputs[offset + k].id));
  } else {
    const auto D = lie<A>::storage_D_tangent(std::get<I>(args));
    Buffer<std::pair<id_t, id_t>> seeds;
    for (std::size_t t = 0; t < lie<A>::tangent_dim; ++t) {
      seeds.clear();
      for (std::size_t k = 0; k < sdim; ++k) {
        const id_t dk = D(k, t).node(g);
        if (!g.is_const(dk, 0.0)) seeds.emplace_back(inputs[offset + k].id, dk);
      }
      cols.push_back(g.diff(roots, seeds));
    }
  }
}

template <auto F, Mode M, class WrtT, class... Args>
struct Builder;

template <auto F, Mode M, std::size_t... W, class... Args>
struct Builder<F, M, Wrt<W...>, Args...> {
  static constexpr bool kOptimize = is_optimized<std::remove_cvref_t<decltype(F)>>;
  using SymArgs = std::tuple<rebind_t<Args, Expr>...>;
  static constexpr auto offsets = arg_offsets<SymArgs>(std::index_sequence_for<Args...>{});
  static constexpr std::size_t num_inputs = offsets[sizeof...(Args)];

  // Default simplification: factor sums in place (Horner, no expansion) where that is cheaper.
  // optimized<f> additionally tries expanding (see Optimized).
  static constexpr Buffer<id_t> simplify(Graph& g, const Buffer<id_t>& keep, const Buffer<id_t>& roots) {
    if constexpr (kOptimize) return optimize(g, keep, roots);
    else return select_cheaper(g, keep, roots, factor_sums(g, roots));
  }

  static constexpr LoweredProgram build() {
    Graph g;
    Buffer<Expr> inputs;
    for (std::size_t k = 0; k < num_inputs; ++k) inputs.emplace_back(&g, g.variable());
    const SymArgs args = [&]<std::size_t... I>(std::index_sequence<I...>) {
      return SymArgs{storage<std::tuple_element_t<I, SymArgs>>::from(inputs.data() + offsets[I])...};
    }(std::index_sequence_for<Args...>{});

    const auto out = std::apply([](const auto&... a) { return F(present(a)...); }, args);
    using Out = std::remove_cvref_t<decltype(out)>;
    std::array<Expr, storage_dim<Out>> flat{};
    storage<Out>::to(out, flat.data());
    Buffer<id_t> roots;
    for (const auto& e : flat) roots.push_back(e.node(g));

    if constexpr (M == Mode::Value) {
      return lower_best(g, roots, simplify(g, {}, roots), Buffer<std::uint32_t>{});
    } else {
      // Tangent columns w.r.t. each argument (rows = output storage), then rows mapped to the
      // output's tangent space.
      constexpr std::size_t mt = output_tangent<Out>::dim;
      Buffer<Buffer<id_t>> cols;
      Buffer<std::uint32_t> col_group;
      {
        std::uint32_t group = 1;
        auto add_arg = [&]<std::size_t I>() {
          const std::size_t before = cols.size();
          append_tangent_columns<I>(g, args, inputs, offsets[I], roots, cols);
          for (std::size_t c = before; c < cols.size(); ++c) col_group.push_back(group);
          ++group;
        };
        (add_arg.template operator()<W>(), ...);
      }
      Buffer<id_t> jac;  // column-major, mt rows per column
      for (const auto& c : cols) {
        Buffer<id_t> t(mt);
        output_tangent<Out>::map(g, out, c.data(), t.data());
        jac.append(t.begin(), t.end());
      }
      const std::size_t n = cols.size();
      Buffer<std::uint32_t> groups(roots.size(), 0);
      for (std::size_t c = 0; c < n; ++c)
        for (std::size_t r = 0; r < mt; ++r) groups.push_back(M == Mode::Jacobian ? col_group[c] : 0);

      // All outputs for given residual roots and Jacobian entries.
      auto outputs = [&](const Buffer<id_t>& res, const Buffer<id_t>& J) {
        Buffer<id_t> all = res;
        all.append(J.begin(), J.end());
        if constexpr (M == Mode::Linearization) {
          static_assert(mt == storage_dim<Out>,
                        "linearize() needs a vector-valued residual; use jacobian() for Lie-group outputs");
          const std::size_t m = res.size();
          // Hessian lower triangle (column-major, i >= j), then rhs.
          Buffer<id_t> terms;
          for (std::size_t j = 0; j < n; ++j)
            for (std::size_t i = j; i < n; ++i) {
              terms.clear();
              for (std::size_t k = 0; k < m; ++k) terms.push_back(g.mul(J[i * m + k], J[j * m + k]));
              all.push_back(g.add(terms));
            }
          for (std::size_t i = 0; i < n; ++i) {
            terms.clear();
            for (std::size_t k = 0; k < m; ++k) terms.push_back(g.mul(J[i * m + k], res[k]));
            all.push_back(g.add(terms));
          }
        }
        return all;
      };
      if constexpr (M == Mode::Linearization) groups.resize(roots.size() + n * mt + n * (n + 1) / 2 + n);
      const Buffer<id_t> plain = outputs(roots, jac);
      const Buffer<id_t> sroots = simplify(g, {}, roots);
      const Buffer<id_t> simplified = outputs(sroots, simplify(g, sroots, jac));
      return lower_best(g, plain, simplified, groups);
    }
  }

  // Lowers both candidate output sets and keeps the one with fewer operations. The simplification
  // passes choose per output with a cost estimate that cannot see what lowering (pair sharing, value
  // numbering) will do; comparing complete programs guarantees simplification never makes things worse.
  static constexpr LoweredProgram lower_best(Graph& g, const Buffer<id_t>& a, const Buffer<id_t>& b,
                                             const Buffer<std::uint32_t>& groups) {
    LoweredProgram pa = lower(g, a, num_inputs, groups);
    bool same = true;
    for (std::size_t i = 0; i < a.size() && same; ++i) same = a[i] == b[i];
    if (same) return pa;
    LoweredProgram pb = lower(g, b, num_inputs, groups);
    return count_ops(pb.instrs.data(), pb.instrs.size()) < count_ops(pa.instrs.data(), pa.instrs.size())
               ? std::move(pb)
               : std::move(pa);
  }

  // Two passes: the first fixes the tape's size, the second fills it. Constant evaluation cannot hand a
  // Buffer to a non-constexpr context, and staging through a large fixed-capacity array is far
  // slower in clang's evaluator than simply rebuilding (see docs/design/csym-constexpr.md). C++26
  // std::define_static_array will remove the second pass.
  static constexpr TapeSizes sizes = [] {
    const auto p = build();
    return TapeSizes{p.instrs.size(), p.consts.size(), p.outputs.size(), p.segments.size()};
  }();

  static constexpr auto tape = [] {
    const auto p = build();
    Tape<sizes.instrs, sizes.consts, sizes.outputs, sizes.segments> t{};
    for (std::size_t i = 0; i < sizes.instrs; ++i) t.instrs[i] = p.instrs[i];
    for (std::size_t i = 0; i < sizes.consts; ++i) t.consts[i] = p.consts[i];
    for (std::size_t i = 0; i < sizes.outputs; ++i) {
      t.outputs[i] = p.outputs[i];
      t.output_groups[i] = p.output_groups[i];
    }
    for (std::size_t i = 0; i < sizes.segments; ++i) t.segments[i] = p.segments[i];
    t.num_inputs = p.num_inputs;
    return t;
  }();
};

// GCC cannot see that a skipped segment's registers are never read (an instruction only runs when every
// operand's segment ran, since operand group masks are supersets), and warns under -Wall.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

// Unrolled evaluator for a tape held in `Holder::tape`. Segments whose group mask does not intersect
// `wanted` are skipped; with wanted == all groups (a constant) every guard folds away.
template <class Holder, class S>
struct Evaluator {
  static constexpr const auto& T = Holder::tape;
  static constexpr std::size_t N = T.num_instrs;
  static constexpr std::size_t K = CSYM_EVAL_CHUNK;
  static_assert(T.num_segments < 256, "too many output-group segments");

  // Output indices grouped by the instruction producing them (counting sort, at compile time), so each
  // output is stored right after its instruction runs. Storing everything at the end would keep every
  // output value live across the whole tape, which spills heavily in large programs.
  static constexpr auto out_first = [] {
    std::array<std::uint32_t, N + 1> first{};
    for (std::size_t j = 0; j < T.num_outputs; ++j) ++first[T.outputs[j] + 1];
    for (std::size_t i = 0; i < N; ++i) first[i + 1] += first[i];
    return first;
  }();
  static constexpr auto out_list = [] {
    std::array<std::uint32_t, T.num_outputs == 0 ? 1 : T.num_outputs> list{};
    auto next = out_first;
    for (std::size_t j = 0; j < T.num_outputs; ++j) list[next[T.outputs[j]]++] = static_cast<std::uint32_t>(j);
    return list;
  }();
  template <std::size_t I, class Sink, std::size_t... L>
  CSYM_ALWAYS_INLINE static constexpr void store_outputs([[maybe_unused]] const S* r,
                                                         [[maybe_unused]] Sink& sink,
                                                         [[maybe_unused]] std::uint32_t wanted,
                                                         std::index_sequence<L...>) {
    ((((wanted >> T.output_groups[out_list[out_first[I] + L]]) & 1u)
          ? sink.template put<out_list[out_first[I] + L]>(r[I])
          : void()),
     ...);
  }

  template <std::size_t I, class Sink>
  CSYM_ALWAYS_INLINE static constexpr void step(S* r, const S* in, Sink& sink, std::uint32_t wanted) {
    compute<I>(r, in);
    store_outputs<I>(r, sink, wanted, std::make_index_sequence<out_first[I + 1] - out_first[I]>{});
  }

  template <std::size_t I>
  CSYM_ALWAYS_INLINE static constexpr void compute(S* r, const S* in) {
    constexpr Instr k = T.instrs[I];
    if constexpr (k.op == IOp::In) r[I] = in[k.a];
    else if constexpr (k.op == IOp::Const) r[I] = static_cast<S>(T.consts[k.a]);
    else if constexpr (k.op == IOp::Add) r[I] = r[k.a] + r[k.b];
    else if constexpr (k.op == IOp::Sub) r[I] = r[k.a] - r[k.b];
    else if constexpr (k.op == IOp::Mul) r[I] = r[k.a] * r[k.b];
    else if constexpr (k.op == IOp::Div) r[I] = r[k.a] / r[k.b];
    else if constexpr (k.op == IOp::Neg) r[I] = -r[k.a];
    else if constexpr (k.op == IOp::Sqrt) r[I] = csym::sqrt(r[k.a]);
    else if constexpr (k.op == IOp::Pow) r[I] = csym::pow(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Sin) r[I] = csym::sin(r[k.a]);
    else if constexpr (k.op == IOp::Cos) r[I] = csym::cos(r[k.a]);
    else if constexpr (k.op == IOp::Tan) r[I] = csym::tan(r[k.a]);
    else if constexpr (k.op == IOp::Asin) r[I] = csym::asin(r[k.a]);
    else if constexpr (k.op == IOp::Acos) r[I] = csym::acos(r[k.a]);
    else if constexpr (k.op == IOp::Atan) r[I] = csym::atan(r[k.a]);
    else if constexpr (k.op == IOp::Exp) r[I] = csym::exp(r[k.a]);
    else if constexpr (k.op == IOp::Log) r[I] = csym::log(r[k.a]);
    else if constexpr (k.op == IOp::Tanh) r[I] = csym::tanh(r[k.a]);
    else if constexpr (k.op == IOp::Abs) r[I] = csym::abs(r[k.a]);
    else if constexpr (k.op == IOp::Sign) r[I] = csym::sign(r[k.a]);
    else if constexpr (k.op == IOp::SignNoZero) r[I] = csym::sign_no_zero(r[k.a]);
    else if constexpr (k.op == IOp::Floor) r[I] = csym::floor(r[k.a]);
    else if constexpr (k.op == IOp::Atan2) r[I] = csym::atan2(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Min) r[I] = csym::min(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Max) r[I] = csym::max(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Lt) r[I] = csym::lt(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Le) r[I] = csym::le(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Eq) r[I] = csym::eq(r[k.a], r[k.b]);
    else if constexpr (k.op == IOp::Where) r[I] = csym::select(r[k.a], r[k.b], r[k.c]);
  }
  template <std::size_t Base, class Sink, std::size_t... J>
  CSYM_ALWAYS_INLINE static constexpr void chunk(S* r, const S* in, Sink& sink, std::uint32_t wanted,
                                                 std::index_sequence<J...>) {
    (step<Base + J>(r, in, sink, wanted), ...);
  }
  template <std::size_t B, std::size_t E, class Sink, std::size_t... C>
  CSYM_ALWAYS_INLINE static constexpr void range(S* r, const S* in, Sink& sink, std::uint32_t wanted,
                                                 std::index_sequence<C...>) {
    (chunk<B + C * K>(r, in, sink, wanted, std::make_index_sequence<(B + C * K + K <= E ? K : E - B - C * K)>{}),
     ...);
  }
  template <std::size_t G, class Sink>
  CSYM_ALWAYS_INLINE static constexpr void segment(S* r, const S* in, Sink& sink, std::uint32_t wanted) {
    constexpr Segment sg = T.segments[G];
    static_assert((sg.end - sg.begin + K - 1) / K < 256, "tape too long for a single-level chunked unroll");
    if (wanted & sg.mask)
      range<sg.begin, sg.end>(r, in, sink, wanted, std::make_index_sequence<(sg.end - sg.begin + K - 1) / K>{});
  }
  template <class Sink, std::size_t... G>
  CSYM_ALWAYS_INLINE static constexpr void run(S* r, const S* in, Sink& sink, std::uint32_t wanted,
                                               std::index_sequence<G...>) {
    (segment<G>(r, in, sink, wanted), ...);
  }
  // Evaluates and hands the outputs of every group set in `wanted` to `sink` (group 0, the value, is
  // always computed).
  template <class Sink>
  CSYM_ALWAYS_INLINE static constexpr void eval(const S* in, Sink& sink, std::uint32_t wanted = ~0u) {
    wanted |= 1u;
    S r[N == 0 ? 1 : N];
    run(r, in, sink, wanted, std::make_index_sequence<T.num_segments>{});
  }
};

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Sink writing output J to blocks[g][J - Starts[g]], where g is J's group: the value and each Jacobian
// block live in separate caller-owned arrays.
template <class Holder, class S, auto Starts>
struct BlockSink {
  S* const* blocks;
  template <std::size_t J>
  CSYM_ALWAYS_INLINE constexpr void put(S v) {
    constexpr std::size_t g = Holder::tape.output_groups[J];
    blocks[g][J - Starts[g]] = v;
  }
};

// Sink writing consecutive output ranges into the arrays of several destinations: output J goes to
// dest[k][J - Starts[k]] for the last k with Starts[k] <= J (resolved at compile time).
template <class S, auto Starts>
struct RangeSink {
  S* const* dest;
  template <std::size_t J>
  CSYM_ALWAYS_INLINE constexpr void put(S v) {
    constexpr std::size_t k = [] {
      std::size_t i = 0;
      while (i + 1 < Starts.size() && Starts[i + 1] <= J) ++i;
      return i;
    }();
    dest[k][J - Starts[k]] = v;
  }
};

// Sink for linearize(): outputs are residual (m), Jacobian (m*n), Hessian lower triangle (column-major),
// rhs (n). Storing lower entry (i, j) also zeroes (j, i), so every Hessian slot is written exactly once.
template <class Holder, class S, std::size_t Mr, std::size_t N>
struct LinearizationSink {
  S *residual, *jacobian, *hessian, *rhs;
  static constexpr std::size_t kJ = Mr, kH = Mr + Mr * N, kR = kH + N * (N + 1) / 2;
  template <std::size_t J>
  CSYM_ALWAYS_INLINE constexpr void put(S v) {
    if constexpr (J < kJ) {
      residual[J] = v;
    } else if constexpr (J < kH) {
      jacobian[J - kJ] = v;
    } else if constexpr (J < kR) {
      constexpr auto ij = [] {  // (i, j) of the k-th lower-triangle entry, column-major
        std::size_t k = J - kH, j = 0;
        while (k >= N - j) k -= N - j++;
        return std::pair<std::size_t, std::size_t>{j + k, j};
      }();
      hessian[ij.second * N + ij.first] = v;
      if constexpr (ij.first != ij.second) hessian[ij.first * N + ij.second] = S(0);  // upper triangle
    } else {
      rhs[J - kR] = v;
    }
  }
};

// Sink writing output J to out[J].
template <class S>
struct ArraySink {
  S* out;
  template <std::size_t J>
  CSYM_ALWAYS_INLINE constexpr void put(S v) {
    out[J] = v;
  }
};

}  // namespace detail

template <auto F, class... Args>
struct Function {
  using S = scalar_of_t<std::tuple_element_t<0, std::tuple<Args...>>>;
  static_assert((std::is_same_v<scalar_of_t<Args>, S> && ...), "all arguments must share a scalar type");
  static_assert(NumericScalar<S>, "Function arguments must be numeric types (float, double or Batch)");

  using SymOutput = std::remove_cvref_t<decltype(F(present(std::declval<rebind_t<Args, Expr>>())...))>;
  using Output = rebind_t<SymOutput, S>;
  static constexpr std::size_t input_dim = (storage_dim<Args> + ... + 0);
  static constexpr std::size_t output_dim = storage_dim<SymOutput>;              // value size
  static constexpr std::size_t output_tangent_dim = output_tangent<SymOutput>::dim;  // Jacobian rows
  using AllWrt = decltype(detail::all_wrt(std::index_sequence_for<Args...>{}));

  template <std::size_t I>
  using Arg = std::tuple_element_t<I, std::tuple<Args...>>;
  // Jacobian block of the output with respect to argument I's tangent space.
  template <std::size_t I>
  using Block = Matrix<S, output_tangent_dim, tangent_dim<Arg<I>>>;

  template <Mode M, class W = AllWrt>
  using Program = detail::Builder<F, M, W, Args...>;

  template <class W>
  static constexpr std::size_t wrt_dim = [] {
    return []<std::size_t... I>(Wrt<I...>) { return (tangent_dim<Arg<I>> + ... + 0); }(W{});
  }();

  // Number of floating-point operations in the generated code for a mode.
  template <Mode M, class W = AllWrt>
  static constexpr std::size_t op_count = Program<M, W>::tape.op_count();

  CSYM_FLATTEN static constexpr Output eval(const Args&... args) {
    const auto out = run<Program<Mode::Value>>(~0u, args...);
    return storage<Output>::from(out.data());
  }

  // Value and tangent-space Jacobian with respect to arguments I... (all arguments if empty), stacked.
  template <std::size_t... I>
  CSYM_FLATTEN static constexpr auto jacobian(const Args&... args) {
    using W = std::conditional_t<sizeof...(I) == 0, AllWrt, Wrt<I...>>;
    constexpr std::size_t n = wrt_dim<W>, mt = output_tangent_dim;
    std::array<S, output_dim> value;
    WithJacobian<Output, S, mt, n> r;
    // Outputs are the value's storage then the stacked Jacobian: write both in place.
    static constexpr std::array<std::size_t, 2> starts{0, output_dim};
    S* dest[2] = {value.data(), r.jacobian.data.data()};
    detail::RangeSink<S, starts> sink{dest};
    const auto in = pack(args...);
    detail::Evaluator<Program<Mode::Jacobian, W>, S>::eval(in.data(), sink, ~0u);
    r.value = storage<Output>::from(value.data());
    return r;
  }

  // Value, plus the Jacobian block for each argument I_k whose pointer H_k is non-null. Blocks that are
  // not requested are not computed (their instructions are skipped). This is the shape GTSAM's
  // evaluateError(x1, x2, H1, H2) wants.
  template <std::size_t... I>
  CSYM_FLATTEN static constexpr Output evaluate(const Args&... args, Block<I>*... H) {
    std::array<S, output_dim> value;  // group 0 is always written
    const auto in = pack(args...);
    evaluate_raw<I...>(in.data(), value.data(), (H ? H->data.data() : nullptr)...);
    return storage<Output>::from(value.data());
  }

  // Lowest-level entry point, for adapters that manage their own memory (e.g. GTSAM factors):
  // reads the packed input `in` (arguments' storage, concatenated at input_offsets), writes the value's
  // storage to `value` and, for each argument I_k whose `block` pointer is non-null, its Jacobian block
  // (output_tangent_dim x tangent_dim of I_k, column-major) directly to that memory. Blocks passed as null
  // are not computed.
  template <std::size_t... I>
  CSYM_FLATTEN static constexpr void evaluate_raw(const S* in, S* value, detail::pointer_for<I, S>... block) {
    using P = Program<Mode::Jacobian, Wrt<I...>>;
    std::array<S*, sizeof...(I) + 1> blocks{value, block...};
    std::uint32_t wanted = 1u;
    for (std::size_t b = 1; b < blocks.size(); ++b)
      if (blocks[b]) wanted |= 1u << b;
    // Output J goes straight to its destination: group = block index, offset within the block known at
    // compile time (outputs are the value followed by each block, column-major).
    static constexpr std::array<std::size_t, sizeof...(I) + 1> starts = [] {
      std::array<std::size_t, sizeof...(I) + 1> st{};
      st[0] = 0;
      std::size_t b = 1, at = output_dim;
      ((st[b++] = at, at += output_tangent_dim * tangent_dim<Arg<I>>), ...);
      return st;
    }();
    detail::BlockSink<P, S, starts> sink{blocks.data()};
    // The two common requests (all blocks: linearizing; none: evaluating the error) get fully
    // specialized code with no runtime guards; partial requests skip segments at runtime.
    constexpr std::uint32_t all = (2u << sizeof...(I)) - 1u;
    if (wanted == all) detail::Evaluator<P, S>::eval(in, sink, all);
    else detail::Evaluator<P, S>::eval(in, sink, wanted);
  }

  // Offset of each argument's storage within the packed input (last entry: input_dim).
  static constexpr std::array<std::size_t, sizeof...(Args) + 1> input_offsets = [] {
    std::array<std::size_t, sizeof...(Args) + 1> o{};
    std::size_t k = 0, at = 0;
    ((o[k++] = at, at += storage_dim<Args>), ...);
    o[k] = at;
    return o;
  }();

  // Residual, Jacobian, JᵀJ and Jᵀr with respect to arguments I... (all arguments if empty).
  template <std::size_t... I>
  CSYM_FLATTEN static constexpr auto linearize(const Args&... args) {
    using W = std::conditional_t<sizeof...(I) == 0, AllWrt, Wrt<I...>>;
    constexpr std::size_t n = wrt_dim<W>, m = output_dim;
    using P = Program<Mode::Linearization, W>;
    Linearization<S, m, n> r;
    detail::LinearizationSink<P, S, m, n> sink{r.residual.data.data(), r.jacobian.data.data(),
                                               r.hessian.data.data(), r.rhs.data.data()};
    const auto in = pack(args...);
    detail::Evaluator<P, S>::eval(in.data(), sink, ~0u);
    return r;
  }

 private:
  // Scratch arrays are default-initialized (not zeroed): every element is written before it is read.
  CSYM_ALWAYS_INLINE static constexpr auto pack(const Args&... args) {
    std::array<S, input_dim == 0 ? 1 : input_dim> in;
    if constexpr (input_dim == 0) in[0] = S(0);
    std::size_t off = 0;
    ((storage<Args>::to(args, in.data() + off), off += storage_dim<Args>), ...);
    return in;
  }
  template <class P>
  CSYM_ALWAYS_INLINE static constexpr auto run(std::uint32_t wanted, const Args&... args) {
    const auto in = pack(args...);
    std::array<S, P::tape.num_outputs> out;  // run() is only used with every group requested
    detail::ArraySink<S> sink{out.data()};
    detail::Evaluator<P, S>::eval(in.data(), sink, wanted);
    return out;
  }
};

}  // namespace csym
