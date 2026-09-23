// Model<N>: the coefficients of a degree-N spherical-harmonic field model plus
// its header metadata. Plain data, valid C++20.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "wmm/types.h"

namespace wmm {

// Number of (n, m) slots for degrees 0..nmax, indexed by n(n+1)/2 + m.
constexpr std::size_t num_terms(int nmax) {
    return static_cast<std::size_t>((nmax + 1) * (nmax + 2) / 2);
}

constexpr std::size_t term_index(int n, int m) {
    return static_cast<std::size_t>(n * (n + 1) / 2 + m);
}

template <int NMax>
struct Model {
    static_assert(NMax >= 1);
    static constexpr int nmax = NMax;

    double epoch = 0.0;        // decimal year the coefficients are referenced to
    std::array<char, 32> name_buf{};
    std::size_t name_len = 0;
    Date release_date{};
    // Row-major over n, then m; slot 0 (n = 0) is unused and always zero.
    std::array<Coefficient, num_terms(NMax)> coeffs{};

    constexpr std::string_view name() const { return {name_buf.data(), name_len}; }
    constexpr const Coefficient& operator()(int n, int m) const { return coeffs[term_index(n, m)]; }

    // Validity window used by the NOAA tools: release date through epoch + 5 years.
    constexpr double valid_from() const { return decimal_year(release_date); }
    constexpr double valid_until() const { return epoch + 5.0; }
};

}  // namespace wmm
