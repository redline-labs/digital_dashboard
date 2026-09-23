---
title: csym constant-evaluation limits
parent: Design notes
---

# csym constant-evaluation limits

## Overview

What constant evaluation costs, measured while building
[csym](../libs/csym.html) as a standalone project (`sym_computation`) before it
moved into this tree on 2026-09-23. It is the reason for the library's odd
choices: its own growable buffer instead of `std::vector`, hand-written sorts,
a two-pass tape, chunked folds, and the step-limit flags on its interface
target. The numbers are from that project, on Apple clang 21 and GCC 16 in
`-std=c++2c`. In this tree the library builds as C++23 on clang and passes a
strict syntax-only check on GCC 15; nothing below was re-measured there.

## Toolchain

Toolchain: Apple clang 21.0.0 (clang-2100.3.34.2), arm64, Apple libc++. Language mode: `-std=c++2c`.
The probes lived in the standalone project's `bench/limits/` and were not brought over.

## Language/library features
| Feature | c++23 | c++2c |
|---|---|---|
| constexpr `std::vector`, `std::string`, `std::unique_ptr`, `std::sort` | yes | yes |
| `if consteval`, static constexpr locals | yes | yes |
| class-type NTTPs (`template <Instr I>`, `template <auto Arr>` of `std::array<struct>`) | yes | yes |
| constexpr placement new (P2747), `static_cast` from `void*` (P2738) | – | yes (`__cpp_constexpr=202406`) |
| `std::define_static_array` (P3491) | – | **no** |
| constexpr exceptions (P3068) | – | no |
| reflection (P2996) | – | no |
| constexpr `<cmath>` (P1383) | – | no → we ship our own constexpr math |

Consequence: we lower the tape in two passes (a consteval size query, then a fill of `std::array<Instr, N>`).
When clang supports `define_static_array`, this collapses to one pass.

## Graph construction (hash-consed DAG in consteval, `graph_size.cpp`)
| nodes | compile time | peak RSS |
|---|---|---|
| ~4k | 0.7 s | 78 MB |
| ~40k | 5.5 s | 139 MB |
| ~120k | 19.6 s | 337 MB |
| ~240k | 40 s | 600 MB |

Time and memory scale linearly, at roughly 6–7k nodes/s.
**The default `-fconstexpr-steps` (1,048,576) only allows a few hundred nodes**, so the library target
sets `-fconstexpr-steps=2000000000` (or larger) as an INTERFACE compile option.
A fixed-size hash table that fills up makes probing loop forever, and that shows up as a step-limit error: the table must grow.

## Unrolled evaluation (`unroll.cpp`)
- A single pack fold over N instructions fails at N > 256 (`expression nesting limit`, i.e. `-fbracket-depth`).
  **Fold in chunks of ≤ 256.**
- 20k-instruction tape: 1.7 s at -O2.
- Without forced inlining, the chunk functions are outlined and the register array spills to memory (230 extra stores for a 300-instruction tape).
  With `[[gnu::always_inline]]` on step/chunk/run, the output is **identical in instruction count and mix to handwritten straight-line
  C++** (347 instructions, same fadd/fmul/sin calls, same single store).

## Sizing for the target use case
A tire or dynamics factor with a full Jacobian is expected to be ~1–5k graph nodes, which takes well under 1 s of constexpr time per factor.
Both the graph and the tape have plenty of headroom.

## Cost model of constant evaluation (found while building the engine)

After Phase 0 the language mode moved to `-std=c++2c`. GCC 16 (`-std=c++26`) builds and passes everything too.

### What is expensive in the evaluator
Micro-benchmarks (`clang 21`, 100k iterations each):

| construct | cost per operation |
|---|---|
| raw `new T[]` write / indexed read | ~2 µs |
| `std::vector::operator[]` read | ~3–6 µs |
| `std::vector::push_back` (capacity reserved) | ~28 µs |
| fresh `std::vector` + reserve | ~100 µs |
| `new[]` + `delete[]` pair | ~16 µs |

The evaluator interprets every layer of libc++'s vector (allocator traits, `construct_at`, annotations).
The symbolic passes therefore use `csym::Buffer<T>` (core/buffer.h): a minimal growable array on raw
`new[]`. Hot paths keep their temporaries on scratch stacks owned by the graph, and use hand-written
insertion and merge sorts instead of `std::sort`. Together with the algorithmic changes below, this took a
`rot3_local` Jacobian build from 8.3 s to 0.5 s.

### Approaches that did not work
- **Staging the tape through a large fixed-capacity array** (to avoid building twice): a `static constexpr`
  struct holding `std::array<Instr, 8192>` made one function take **756 s** instead of 10 s. Clang's evaluator
  is very slow with large arrays of structs in constants. Building twice is far cheaper.
- **`-fexperimental-new-constant-interpreter`** (clang's bytecode interpreter): after working around one bug,
  it was only ~10% faster. It is also clang-specific, so it is not used. The workaround (`csym::pick` is a
  namespace-scope function) is kept because it is ordinary C++.
- **Expanding and re-factoring every output polynomial by default**: it helped one geometry benchmark by ~10%
  and cost 10x the compile time. It is opt-in: `Function<csym::optimized<f>, ...>`.

### What made the generated code good *and* cheap to build
1. **Tangent-seeded forward differentiation**: a Lie-group argument's input variables are seeded with the
   columns of `storage_D_tangent` instead of computing a storage Jacobian and multiplying afterwards.
   Intermediate results get their own tangent derivatives, which are shared across outputs. `rot3_local`
   went from 542 to 261 ops.
2. **Pair sharing at lowering**: canonical n-ary products lose sub-products (e.g. `sin²/n²` recomputed in six
   products). Before emitting, every product is planned as a list of (possibly inverted) atoms, and element
   pairs occurring in several products are hoisted into shared temporaries, most frequent first, in up to 3
   rounds. This took `rot3_local` to 180 ops (SymForce: 199) and Pose3 between to 1457 (SymForce: 1388).

### Compile-time scaling today (RK4 bicycle factor, 12 tangent columns)
| mode | ops | clang 21 | GCC 16 |
|---|---|---|---|
| residual | 312 | 0.5 s / 78 MB | 0.5 s / 197 MB |
| + Jacobian | 1339 | 2.9 s / 95 MB | 1.8 s / 510 MB |
| + JᵀJ, Jᵀr | 1603 | 3.5 s / 100 MB | 2.1 s / 593 MB |

(`-O0` builds of a single translation unit containing only that function; every program is built twice.)

## Round 2: generated-code quality and runtime

- **Unrolled output stores.** Copying outputs with a runtime loop (`out[j] = r[outputs[j]]`) forced the
  whole register array into memory once there were too many outputs for the optimizer to unroll (78 for the
  bicycle Jacobian). The result was 140 stack stores and a Jacobian ~3x slower than its value. Outputs are now
  written through a sink with compile-time indices. Bicycle factor on clang: stacked Jacobian 120 → 38 ns,
  linearization 234 → ~124 ns.
- **Shared reciprocals.** A denominator used by several products (including inside hoisted quotient pairs)
  is inverted once. `rot3_from_tangent`'s Jacobian went from 20 divisions to 4.
- **In-place sum factoring** (Horner over existing terms, including negative exponents), kept only when the
  complete lowered program is smaller. Pose3 between Jacobian 1457 → 1096 ops, prior 590 → 375.
- **Output-group segments** (value, Jacobian block per argument) let `evaluate()` skip unrequested blocks.
  Instructions are ordered by descending popcount of their group mask; operand masks are supersets, so this
  preserves topological order. All-blocks requests use a specialized evaluator with no runtime guards.
- **Compilers and transcendentals.** clang on macOS fuses sin/cos pairs into `__sincos_stret`. GCC 16 on
  macOS lowers them to `cexp`, which is slower. GCC also does not scalarize the register array as well for
  the largest programs (GCC's `--param` limits help, but a header cannot set them).

## Round 3: batched (SIMD) evaluation

- **The auto-vectorizer is not enough.** Lane loops over a `T[4]` vectorize for plain arithmetic once
  inlined (clang), but not for the transcendental kernels. Their 4-iteration loops around large
  branch-free bodies stay scalar, and GCC vectorizes even less. Lanes are now GNU `vector_size` vectors
  (GCC and clang implement them identically, including vector `?:` selects, `__builtin_convertvector` and
  constexpr evaluation). The kernels are templates written once for `double` (constant evaluation,
  fallback) and for the native vector type.
- **GCC's constant evaluator rejects single-element writes to vector-extension values.** Lanes are built
  with brace initializers (`Batch::from_fn`).
- **Gains are bounded by the hardware.** On Apple M-series (128-bit NEON) the residual improves 1.2–1.3x
  and residual + blocks ~1.3x with clang; full linearization does not improve because of register pressure.
  Reordering instructions depth-first to shorten live ranges made no measurable difference (the compiler
  reschedules anyway), so it was not kept.
