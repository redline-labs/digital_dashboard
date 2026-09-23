#pragma once

// Buffer<T>: a minimal growable array for use during constant evaluation.
//
// std::vector works in constexpr, but constant evaluators interpret every layer of its implementation
// (allocator traits, construct_at, annotations, ...). Measured in clang 21, push_back costs ~28 µs and a
// fresh vector ~100 µs, against ~2 µs for a raw array write (docs/design/csym-constexpr.md). The symbolic passes
// are built on this type instead; it keeps the operations they need and nothing else.
//
// T must be default-constructible and copyable (ids, PODs, pairs).

#include <cstddef>
#include <initializer_list>
#include <span>
#include <utility>

namespace csym {

template <class T>
class Buffer {
 public:
  constexpr Buffer() = default;
  constexpr explicit Buffer(std::size_t n) { resize(n); }
  constexpr Buffer(std::size_t n, const T& v) {
    reserve(n);
    for (std::size_t i = 0; i < n; ++i) data_[i] = v;
    size_ = n;
  }
  constexpr Buffer(const T* first, const T* last) {
    reserve(static_cast<std::size_t>(last - first));
    for (; first != last; ++first) data_[size_++] = *first;
  }
  constexpr Buffer(std::initializer_list<T> l) : Buffer(l.begin(), l.end()) {}
  constexpr Buffer(const Buffer& o) : Buffer(o.begin(), o.end()) {}
  constexpr Buffer(Buffer&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) {
    o.data_ = nullptr;
    o.size_ = o.cap_ = 0;
  }
  constexpr Buffer& operator=(const Buffer& o) {
    if (this != &o) {
      size_ = 0;
      reserve(o.size_);
      for (std::size_t i = 0; i < o.size_; ++i) data_[i] = o.data_[i];
      size_ = o.size_;
    }
    return *this;
  }
  constexpr Buffer& operator=(Buffer&& o) noexcept {
    swap(o);
    return *this;
  }
  constexpr ~Buffer() { delete[] data_; }

  constexpr void swap(Buffer& o) noexcept {
    std::swap(data_, o.data_);
    std::swap(size_, o.size_);
    std::swap(cap_, o.cap_);
  }

  constexpr void reserve(std::size_t n) {
    if (n <= cap_) return;
    T* d = new T[n];
    for (std::size_t i = 0; i < size_; ++i) d[i] = std::move(data_[i]);
    delete[] data_;
    data_ = d;
    cap_ = n;
  }
  constexpr void push_back(const T& v) {
    if (size_ == cap_) {
      const T copy = v;  // v may alias an element that reserve() is about to free
      reserve(cap_ ? cap_ * 2 : 8);
      data_[size_++] = copy;
      return;
    }
    data_[size_++] = v;
  }
  constexpr void push_back(T&& v) {
    if (size_ == cap_) {
      T moved = std::move(v);
      reserve(cap_ ? cap_ * 2 : 8);
      data_[size_++] = std::move(moved);
      return;
    }
    data_[size_++] = std::move(v);
  }
  template <class... A>
  constexpr void emplace_back(A&&... a) {
    push_back(T{std::forward<A>(a)...});
  }
  constexpr void pop_back() { --size_; }
  constexpr void clear() { size_ = 0; }
  // New elements are value-initialized.
  constexpr void resize(std::size_t n) {
    reserve(n);
    for (std::size_t i = size_; i < n; ++i) data_[i] = T{};
    size_ = n;
  }
  constexpr void assign(const T* first, const T* last) {
    clear();
    reserve(static_cast<std::size_t>(last - first));
    for (; first != last; ++first) data_[size_++] = *first;
  }
  constexpr void append(const T* first, const T* last) {
    reserve(size_ + static_cast<std::size_t>(last - first));
    for (; first != last; ++first) data_[size_++] = *first;
  }

  constexpr std::size_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }
  constexpr T& operator[](std::size_t i) { return data_[i]; }
  constexpr const T& operator[](std::size_t i) const { return data_[i]; }
  constexpr T& back() { return data_[size_ - 1]; }
  constexpr const T& back() const { return data_[size_ - 1]; }
  constexpr T* data() { return data_; }
  constexpr const T* data() const { return data_; }
  constexpr T* begin() { return data_; }
  constexpr T* end() { return data_ + size_; }
  constexpr const T* begin() const { return data_; }
  constexpr const T* end() const { return data_ + size_; }
  // No conversion operator to std::span: Buffer is a contiguous sized range, so
  // span's own range constructor already converts it, and GCC's -Wconversion
  // flags every call site where both candidates exist.

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0, cap_ = 0;
};

// Stable merge sort of a Buffer (std::sort works in constexpr too, but is costly to interpret).
template <class T, class Less>
constexpr void sort(Buffer<T>& v, Less less) {
  const std::size_t n = v.size();
  if (n < 2) return;
  Buffer<T> tmp(n);
  T* a = v.data();
  T* b = tmp.data();
  for (std::size_t width = 1; width < n; width *= 2) {
    for (std::size_t lo = 0; lo < n; lo += 2 * width) {
      const std::size_t mid = lo + width < n ? lo + width : n;
      const std::size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      std::size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) b[k++] = less(a[j], a[i]) ? a[j++] : a[i++];
      while (i < mid) b[k++] = a[i++];
      while (j < hi) b[k++] = a[j++];
    }
    T* t = a;
    a = b;
    b = t;
  }
  if (a != v.data())
    for (std::size_t i = 0; i < n; ++i) v[i] = a[i];
}

}  // namespace csym
