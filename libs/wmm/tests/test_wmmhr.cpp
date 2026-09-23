// Checks WMMHR2025 against the NCEI published test values
// (tests/data/WMMHR2025_TEST_VALUES.txt), both during constant evaluation
// (static_assert) and at run time.
#include <cstdio>
#include <limits>

#include "embedded_model.h"

namespace {

constexpr double nan = std::numeric_limits<double>::quiet_NaN();

struct TestPoint {
    double year, height_km, lat, lon;
    double X, Y, Z, H, F, I, D, GV;
    double X_dot, Y_dot, Z_dot, H_dot, F_dot, I_dot, D_dot;
};

// Verbatim from WMMHR2025_TEST_VALUES.txt.
constexpr TestPoint ncei_test_values[] = {
    {2025.0, 0.0, 80.0, 0.0, 6517.4, 144.8, 54701.3, 6519.0, 55088.3, 83.20, 1.27, 1.27, -9.0, 59.5, 31.1, -7.7, 29.9, 0.01, 0.52},
    {2025.0, 0.0, 0.0, 120.0, 39643.1, -100.3, -10580.7, 39643.2, 41030.9, -14.94, -0.14, nan, 9.6, -23.5, 78.7, 9.6, -11.0, 0.11, -0.03},
    {2025.0, 0.0, -80.0, 240.0, 6136.3, 15740.2, -52096.7, 16894.0, 54767.4, -72.03, 68.70, -51.30, 32.9, -8.2, 94.4, 4.3, -88.4, 0.03, -0.11},
    {2025.0, 100.0, 80.0, 0.0, 6218.6, 81.8, 52567.3, 6219.1, 52933.9, 83.25, 0.75, 0.75, -8.3, 56.6, 28.6, -7.6, 27.5, 0.01, 0.52},
    {2025.0, 100.0, 0.0, 120.0, 37679.1, -98.7, -10148.9, 37679.3, 39022.1, -15.07, -0.15, nan, 9.2, -21.3, 72.3, 9.2, -9.9, 0.11, -0.03},
    {2025.0, 100.0, -80.0, 240.0, 5916.0, 14762.5, -49580.3, 15903.8, 52068.5, -72.22, 68.16, -51.84, 30.2, -7.7, 88.2, 4.1, -82.8, 0.03, -0.11},
    {2027.5, 0.0, 80.0, 0.0, 6494.8, 293.5, 54779.0, 6501.5, 55163.4, 83.23, 2.59, 2.59, -9.0, 59.5, 31.1, -6.3, 30.1, 0.01, 0.53},
    {2027.5, 0.0, 0.0, 120.0, 39667.0, -159.1, -10383.9, 39667.3, 41003.9, -14.67, -0.23, nan, 9.6, -23.5, 78.7, 9.7, -10.6, 0.11, -0.03},
    {2027.5, 0.0, -80.0, 240.0, 6218.5, 15719.7, -51860.8, 16905.0, 54546.5, -71.95, 68.42, -51.58, 32.9, -8.2, 94.4, 4.5, -88.3, 0.04, -0.11},
    {2027.5, 100.0, 80.0, 0.0, 6197.8, 223.2, 52638.8, 6201.8, 53002.9, 83.28, 2.06, 2.06, -8.3, 56.6, 28.6, -6.3, 27.7, 0.01, 0.52},
    {2027.5, 100.0, 0.0, 120.0, 37702.1, -152.0, -9968.1, 37702.4, 38997.9, -14.81, -0.23, nan, 9.2, -21.3, 72.3, 9.3, -9.5, 0.11, -0.03},
    {2027.5, 100.0, -80.0, 240.0, 5991.6, 14743.3, -49359.7, 15914.3, 51861.8, -72.13, 67.88, -52.12, 30.2, -7.7, 88.2, 4.3, -82.7, 0.03, -0.11},
};

// The published values are rounded to 0.1 nT and 0.01 degrees.
constexpr double nt_tol = 0.05 + 1e-6;
constexpr double deg_tol = 0.005 + 1e-6;

constexpr bool near(double actual, double expected, double tol) {
    if (expected != expected) return actual != actual;  // NaN expected
    const double d = actual - expected;
    return d <= tol && -d <= tol;
}

constexpr wmm::MagneticElements evaluate(const TestPoint& t) {
    return wmm::magnetic_field(test::wmmhr2025, {t.lat, t.lon, t.height_km}, t.year);
}

// Bit mask of mismatching fields (0 = all match), in the file's column order.
constexpr unsigned mismatches(const TestPoint& t, const wmm::MagneticElements& e) {
    const double actual[] = {e.X, e.Y, e.Z, e.H, e.F, e.I, e.D, e.GV,
                             e.X_dot, e.Y_dot, e.Z_dot, e.H_dot, e.F_dot, e.I_dot, e.D_dot};
    const double expected[] = {t.X, t.Y, t.Z, t.H, t.F, t.I, t.D, t.GV,
                               t.X_dot, t.Y_dot, t.Z_dot, t.H_dot, t.F_dot, t.I_dot, t.D_dot};
    constexpr bool is_angle[] = {false, false, false, false, false, true, true, true,
                                 false, false, false, false, false, true, true};
    unsigned mask = 0;
    for (int i = 0; i < 15; ++i)
        if (!near(actual[i], expected[i], is_angle[i] ? deg_tol : nt_tol)) mask |= 1u << i;
    return mask;
}

constexpr bool matches(int row) {
    return mismatches(ncei_test_values[row], evaluate(ncei_test_values[row])) == 0;
}

// Each static_assert is a separate constant evaluation (one full degree-133
// synthesis each), which keeps every one within the default step budget.
static_assert(matches(0));
static_assert(matches(1));
static_assert(matches(2));
static_assert(matches(3));
static_assert(matches(4));
static_assert(matches(5));
static_assert(matches(6));
static_assert(matches(7));
static_assert(matches(8));
static_assert(matches(9));
static_assert(matches(10));
static_assert(matches(11));

// Compile-time results, captured for comparison with the run-time path. One
// variable per point: each is a separate constant evaluation.
template <std::size_t I>
constexpr wmm::MagneticElements compile_time_result = evaluate(ncei_test_values[I]);

template <std::size_t... I>
constexpr std::array<wmm::MagneticElements, sizeof...(I)> compile_time_results(std::index_sequence<I...>) {
    return {compile_time_result<I>...};
}

// Pole: the By series needs the special summation; it must stay finite.
constexpr auto north_pole = wmm::magnetic_field(test::wmmhr2025, {90.0, 0.0, 0.0}, 2026.0);
static_assert(north_pole.F > 50000 && north_pole.F < 60000);

static_assert(wmm::decimal_year({2025, 1, 1}) == 2025.0);
static_assert(wmm::decimal_year({2028, 7, 1}) == 2028.0 + 182.0 / 366.0);
static_assert(test::wmmhr2025.valid_from() == wmm::decimal_year({2024, 11, 13}));
static_assert(test::wmmhr2025.valid_until() == 2030.0);

}  // namespace

int main() {
    constexpr std::size_t count = std::size(ncei_test_values);
    constexpr auto compile_time = compile_time_results(std::make_index_sequence<count>{});

    std::size_t failures = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& t = ncei_test_values[i];
        const auto e = evaluate(t);
        if (unsigned mask = mismatches(t, e)) {
            std::printf("FAIL row %zu (mask 0x%x): X=%.2f Y=%.2f Z=%.2f D=%.4f I=%.4f GV=%.4f\n", i, mask, e.X, e.Y,
                        e.Z, e.D, e.I, e.GV);
            ++failures;
        }
    }
    double max_diff = 0;
    for (std::size_t i = 0; i < compile_time.size(); ++i) {
        const auto rt = evaluate(ncei_test_values[i]);
        const auto& ct = compile_time[i];
        for (double d : {rt.X - ct.X, rt.Y - ct.Y, rt.Z - ct.Z, rt.X_dot - ct.X_dot, rt.Y_dot - ct.Y_dot,
                         rt.Z_dot - ct.Z_dot})
            max_diff = d < 0 ? (-d > max_diff ? -d : max_diff) : (d > max_diff ? d : max_diff);
    }
    std::printf("%zu/%zu NCEI test points match; compile-time vs run-time max |diff| = %.3g nT\n",
                count - failures, count, max_diff);
    if (max_diff > 1e-6) ++failures;
    return failures == 0 ? 0 : 1;
}
