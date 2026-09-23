#pragma once

#include <cstddef>

namespace csym {

// W lanes of T evaluated together (see core/batch.h).
template <class T, std::size_t W>
struct Batch;

template <class T>
inline constexpr bool is_batch_v = false;
template <class T, std::size_t W>
inline constexpr bool is_batch_v<Batch<T, W>> = true;

}  // namespace csym
