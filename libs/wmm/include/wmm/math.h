// Minimal constexpr replacements for the <cmath> functions the field synthesis
// needs. Clang (as of 21) cannot constant-fold sqrt/sin/atan etc., so under
// `if consteval` these fall back to portable implementations (fdlibm-style
// kernels, accurate to ~1 ulp over the ranges used here); at run time they call
// the platform libm.
#pragma once

#include <cmath>
#include <limits>
#include <numbers>

namespace wmm::math {
namespace ce {

constexpr double abs(double x) { return x < 0 ? -x : x; }

constexpr double sqrt(double x) {
    if (x <= 0.0) return 0.0;  // callers never pass negative values
    if (x == std::numeric_limits<double>::infinity()) return x;
    // Scale into [1, 4) by powers of 4 (exact), then Newton-Raphson.
    double scale = 1.0;
    while (x >= 4.0) { x *= 0.25; scale *= 2.0; }
    while (x < 1.0) { x *= 4.0; scale *= 0.5; }
    double y = 0.5 * (1.0 + x);
    for (int i = 0; i < 6; ++i) y = 0.5 * (y + x / y);
    return y * scale;
}

constexpr double round_to_int(double x) {
    return static_cast<double>(static_cast<long long>(x < 0 ? x - 0.5 : x + 0.5));
}

// sin and cos on [-pi/4, pi/4] (fdlibm __kernel_sin / __kernel_cos).
constexpr double kernel_sin(double x) {
    constexpr double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                     S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                     S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const double z = x * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    return x + z * x * (S1 + z * r);
}

constexpr double kernel_cos(double x) {
    constexpr double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                     C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                     C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + z * r);
}

// Cody-Waite reduction by pi/2 (three 33-bit pieces; exact for |k| < 2^20, far
// beyond the angles used here). Returns the quadrant in k.
constexpr double reduce_pio2(double x, long long& k) {
    constexpr double pio2_1 = 1.57079632673412561417e+00, pio2_2 = 6.07710050630396597660e-11,
                     pio2_3 = 2.02226624871116645580e-21, two_over_pi = 6.36619772367581382433e-01;
    const double fk = round_to_int(x * two_over_pi);
    k = static_cast<long long>(fk);
    return ((x - fk * pio2_1) - fk * pio2_2) - fk * pio2_3;
}

constexpr double sin(double x) {
    long long k = 0;
    const double r = reduce_pio2(x, k);
    switch (k & 3) {
        case 0: return kernel_sin(r);
        case 1: return kernel_cos(r);
        case 2: return -kernel_sin(r);
        default: return -kernel_cos(r);
    }
}

constexpr double cos(double x) {
    long long k = 0;
    const double r = reduce_pio2(x, k);
    switch (k & 3) {
        case 0: return kernel_cos(r);
        case 1: return -kernel_sin(r);
        case 2: return -kernel_cos(r);
        default: return kernel_sin(r);
    }
}

// fdlibm s_atan.c
constexpr double atan(double x) {
    constexpr double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                 9.82793723247329054082e-01, 1.57079632679489655800e+00};
    constexpr double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                 1.39033110312309984516e-17, 6.12323399573676603587e-17};
    constexpr double aT[] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01,
                             1.42857142725034663711e-01,  -1.11111104054623557880e-01,
                             9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                             6.66107313738753120669e-02,  -5.83357013379057348645e-02,
                             4.97687799461593236017e-02,  -3.65315727442169155270e-02,
                             1.62858201153657823623e-02};
    const bool negative = x < 0;
    double ax = negative ? -x : x;
    if (ax > 1e17) return negative ? -atanhi[3] : atanhi[3];
    int id = -1;
    if (ax >= 0.4375) {
        if (ax < 1.1875) {
            if (ax < 0.6875) { id = 0; ax = (2.0 * ax - 1.0) / (2.0 + ax); }
            else             { id = 1; ax = (ax - 1.0) / (ax + 1.0); }
        } else {
            if (ax < 2.4375) { id = 2; ax = (ax - 1.5) / (1.0 + 1.5 * ax); }
            else             { id = 3; ax = -1.0 / ax; }
        }
    }
    const double z = ax * ax;
    const double w = z * z;
    const double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    const double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    double r = id < 0 ? ax - ax * (s1 + s2)
                      : atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
    return negative ? -r : r;
}

constexpr double atan2(double y, double x) {
    constexpr double pi = std::numbers::pi;
    if (x == 0.0) {
        if (y == 0.0) return 0.0;
        return y > 0 ? pi / 2 : -pi / 2;
    }
    const double a = atan(y / x);
    if (x > 0) return a;
    return y >= 0 ? a + pi : a - pi;
}

constexpr double asin(double x) {
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

}  // namespace ce

constexpr double abs(double x) { return ce::abs(x); }

constexpr double sqrt(double x) {
    if consteval { return ce::sqrt(x); } else { return std::sqrt(x); }
}
constexpr double sin(double x) {
    if consteval { return ce::sin(x); } else { return std::sin(x); }
}
constexpr double cos(double x) {
    if consteval { return ce::cos(x); } else { return std::cos(x); }
}
constexpr double asin(double x) {
    if consteval { return ce::asin(x); } else { return std::asin(x); }
}
constexpr double atan2(double y, double x) {
    if consteval { return ce::atan2(y, x); } else { return std::atan2(y, x); }
}

constexpr double deg2rad(double deg) { return deg * (std::numbers::pi / 180.0); }
constexpr double rad2deg(double rad) { return rad * (180.0 / std::numbers::pi); }

}  // namespace wmm::math
