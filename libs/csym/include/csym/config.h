#pragma once

// Forces inlining of the unrolled tape evaluator. Without it the chunk functions are outlined and the
// register file spills to memory (see docs/design/csym-constexpr.md).
#if defined(__GNUC__) || defined(__clang__)
#define CSYM_ALWAYS_INLINE [[gnu::always_inline]] inline
#else
#define CSYM_ALWAYS_INLINE inline
#endif

// Same, for lambdas: [](auto x) CSYM_LAMBDA_INLINE { ... }
#if defined(__GNUC__) || defined(__clang__)
#define CSYM_LAMBDA_INLINE __attribute__((always_inline))
#else
#define CSYM_LAMBDA_INLINE
#endif

// Inlines everything called from the marked function (recursively). Used on the Function entry points so
// that the generated evaluator and its small helpers (storage packing, math wrappers) form one straight-
// line body; GCC otherwise stops inlining small helpers into very large functions.
#if defined(__GNUC__) || defined(__clang__)
#define CSYM_FLATTEN [[gnu::flatten]]
#else
#define CSYM_FLATTEN
#endif

// Maximum pack size folded at once by the evaluator. Clang limits fold-expression nesting to
// -fbracket-depth (default 256).
#ifndef CSYM_EVAL_CHUNK
#define CSYM_EVAL_CHUNK 128
#endif

