// Magnetic field synthesis for a Model<N> (port of the NCEI geomagnetism library,
// GeomagnetismLibrary.c). Fully constexpr: the same code runs in static_assert
// and at run time.
//
// Differences from the C library's structure (results are the same):
//  * The associated Legendre functions are generated column by column (as in
//    MAG_PcupHigh) or row by row (MAG_PcupLow) and consumed immediately by the
//    summation, so evaluation needs O(nmax) stack instead of O(nmax^2) heap.
//  * Time adjustment of the coefficients happens inside the summation rather
//    than by copying the model.
#pragma once

#include <array>
#include <limits>

#include "geodesy/wgs84.h"
#include "wmm/cof.h"
#include "wmm/math.h"

namespace wmm {

// The WGS-84 ellipsoid, from libs/geodesy so the tree has one, and the
// geomagnetic reference radius (km), which is the model's own.
struct Ellipsoid {
    double a = geodesy::wgs84::kA / 1000.0;  // semi-major axis
    double b = geodesy::wgs84::kB / 1000.0;  // semi-minor axis
    double re = 6371.2;                      // geomagnetic reference radius
    constexpr double eps_sq() const { return 1.0 - (b * b) / (a * a); }
};
inline constexpr Ellipsoid wgs84{};

struct SphericalCoord {
    double longitude_deg = 0.0;
    double latitude_deg = 0.0;  // geocentric latitude
    double radius_km = 0.0;
};

// Geodetic -> geocentric spherical (MAG_GeodeticToSpherical).
constexpr SphericalCoord to_spherical(const GeodeticCoord& p, const Ellipsoid& e = wgs84) {
    const double lat = math::deg2rad(p.latitude_deg);
    const double cos_lat = math::cos(lat);
    const double sin_lat = math::sin(lat);
    const double e2 = e.eps_sq();
    const double rc = e.a / math::sqrt(1.0 - e2 * sin_lat * sin_lat);  // prime vertical radius
    const double xp = (rc + p.height_km) * cos_lat;
    const double zp = (rc * (1.0 - e2) + p.height_km) * sin_lat;
    const double r = math::sqrt(xp * xp + zp * zp);
    return {p.longitude_deg, math::rad2deg(math::asin(zp / r)), r};
}

namespace detail {

// The Legendre recursions are written in integer degree n and order m, which
// appear in the arithmetic as often as in subscripts. This is the one place
// they become indices; every caller keeps 0 <= i < K. K is an int for the
// same reason: it is written N + 1 over the model's int degree.
template <int K>
struct Row {
    std::array<double, static_cast<std::size_t>(K)> v{};

    constexpr double& operator[](int i) { return v[static_cast<std::size_t>(i)]; }
    constexpr const double& operator[](int i) const { return v[static_cast<std::size_t>(i)]; }
};

struct Vec3 {
    double x = 0, y = 0, z = 0;  // north, east, down
};

// Running sums of the spherical-harmonic series for the main field (at the
// requested time) and its secular variation.
template <int N>
struct Synthesis {
    const Model<N>& model;
    double dt;  // years since model epoch
    Row<N + 1> radius_power{};  // (re/r)^(n+2)
    Row<N + 1> cos_ml{};        // cos(m * lambda)
    Row<N + 1> sin_ml{};
    Vec3 field{};
    Vec3 sv{};

    // Adds the (n, m) term given the Schmidt semi-normalised P(n,m)(sin phi) and
    // its derivative with respect to latitude (MAG_Summation, MAG_SecVarSummation).
    constexpr void add(int n, int m, double p, double dp) {
        const Coefficient& c = model.coeffs[term_index(n, m)];
        const double w = radius_power[n];
        const double cm = cos_ml[m], sm = sin_ml[m];

        const double g = c.g + dt * c.g_dot, h = c.h + dt * c.h_dot;
        const double a = g * cm + h * sm;
        field.z -= w * a * (n + 1) * p;
        field.y += w * (g * sm - h * cm) * m * p;
        field.x -= w * a * dp;

        const double sa = c.g_dot * cm + c.h_dot * sm;
        sv.z -= w * sa * (n + 1) * p;
        sv.y += w * (c.g_dot * sm - c.h_dot * cm) * m * p;
        sv.x -= w * sa * dp;
    }

    // Legendre functions by columns of fixed order m, scaled by 1e-280 to avoid
    // underflow at high degree (MAG_PcupHigh, after Holmes & Featherstone 2002).
    // Requires |x| < 1.
    constexpr void sum_high(double x) {
        Row<2 * N + 2> sq{};  // sqrt(0 .. 2N+1)
        for (int i = 0; i <= 2 * N + 1; ++i) sq[i] = math::sqrt(static_cast<double>(i));
        const double z = math::sqrt((1.0 - x) * (1.0 + x));
        constexpr double scalef = 1.0e-280;

        // m = 0
        double pm2 = 1.0;
        double pm1 = x;
        add(1, 0, pm1, z);
        for (int n = 2; n <= N; ++n) {
            const double f1 = static_cast<double>(2 * n - 1) / n;
            const double f2 = static_cast<double>(n - 1) / n;
            const double plm = f1 * x * pm1 - f2 * pm2;
            add(n, 0, plm, n * (pm1 - x * plm) / z);
            pm2 = pm1;
            pm1 = plm;
        }

        double pmm = sq[2] * scalef;
        double rescalem = 1.0 / scalef;
        for (int m = 1; m <= N - 1; ++m) {
            rescalem *= z;
            // P(m, m)
            pmm = pmm * sq[2 * m + 1] / sq[2 * m];
            const double p_mm = pmm * rescalem / sq[2 * m + 1];
            add(m, m, p_mm, -(m * x * p_mm / z));
            // P(m + 1, m)
            pm2 = pmm / sq[2 * m + 1];
            pm1 = x * sq[2 * m + 1] * pm2;
            const double p_m1m = pm1 * rescalem;
            add(m + 1, m, p_m1m, ((pm2 * rescalem) * sq[2 * m + 1] - x * (m + 1) * p_m1m) / z);
            // P(n, m), n >= m + 2
            for (int n = m + 2; n <= N; ++n) {
                const double f1 = (2 * n - 1) / sq[n + m] / sq[n - m];
                const double f2 = sq[n - m - 1] * sq[n + m - 1] / sq[n + m] / sq[n - m];
                const double plm = x * f1 * pm1 - f2 * pm2;
                const double p = plm * rescalem;
                add(n, m, p, (sq[n + m] * sq[n - m] * (pm1 * rescalem) - n * x * p) / z);
                pm2 = pm1;
                pm1 = plm;
            }
        }
        // P(N, N)
        rescalem *= z;
        pmm = pmm / sq[2 * N];
        const double p_nn = pmm * rescalem;
        add(N, N, p_nn, -N * x * p_nn / z);
    }

    // Legendre functions by rows of fixed degree n, Gauss-normalised then
    // converted to Schmidt semi-normalised (MAG_PcupLow). Used for low-degree
    // models and at the poles, where it stays well defined.
    constexpr void sum_low(double x) {
        const double z = math::sqrt((1.0 - x) * (1.0 + x));
        Row<N + 1> p1{}, dp1{}, p2{}, dp2{};  // rows n-1 and n-2 (Gauss-normalised)
        Row<N + 1> p0{}, dp0{};
        p1[0] = 1.0;  // row 0
        double norm0 = 1.0;  // Schmidt factor for (n, 0)
        for (int n = 1; n <= N; ++n) {
            for (int m = 0; m <= n; ++m) {
                if (n == m) {
                    p0[m] = z * p1[m - 1];
                    dp0[m] = z * dp1[m - 1] + x * p1[m - 1];
                } else if (n == 1 || m > n - 2) {
                    p0[m] = x * p1[m];
                    dp0[m] = x * dp1[m] - z * p1[m];
                } else {
                    const double k = static_cast<double>((n - 1) * (n - 1) - m * m) /
                                     static_cast<double>((2 * n - 1) * (2 * n - 3));
                    p0[m] = x * p1[m] - k * p2[m];
                    dp0[m] = x * dp1[m] - z * p1[m] - k * dp2[m];
                }
            }
            norm0 = norm0 * (2 * n - 1) / n;
            double norm = norm0;
            for (int m = 0; m <= n; ++m) {
                if (m > 0) norm *= math::sqrt(static_cast<double>((n - m + 1) * (m == 1 ? 2 : 1)) / (n + m));
                // Sign flip: derivative with respect to latitude, not colatitude.
                add(n, m, p0[m] * norm, -dp0[m] * norm);
            }
            p2 = p1;
            dp2 = dp1;
            p1 = p0;
            dp1 = dp0;
        }
    }

    // East component at the geographic poles, where the series for By has a
    // removable 0/0 (MAG_SummationSpecial, MAG_SecVarSummationSpecial).
    constexpr void sum_pole_east(double sin_phi) {
        Row<N + 1> ps{};
        ps[0] = 1.0;
        double norm1 = 1.0;
        field.y = 0.0;
        sv.y = 0.0;
        for (int n = 1; n <= N; ++n) {
            const double norm2 = norm1 * (2 * n - 1) / n;
            const double norm3 = norm2 * math::sqrt(static_cast<double>(n * 2) / (n + 1));
            norm1 = norm2;
            if (n == 1) {
                ps[n] = ps[n - 1];
            } else {
                const double k = static_cast<double>((n - 1) * (n - 1) - 1) /
                                 static_cast<double>((2 * n - 1) * (2 * n - 3));
                ps[n] = sin_phi * ps[n - 1] - k * ps[n - 2];
            }
            const Coefficient& c = model.coeffs[term_index(n, 1)];
            const double g = c.g + dt * c.g_dot, h = c.h + dt * c.h_dot;
            field.y += radius_power[n] * (g * sin_ml[1] - h * cos_ml[1]) * ps[n] * norm3;
            sv.y += radius_power[n] * (c.g_dot * sin_ml[1] - c.h_dot * cos_ml[1]) * ps[n] * norm3;
        }
    }
};

// Rotate from geocentric to geodetic north/down (MAG_RotateMagneticVector).
constexpr Vec3 rotate(const Vec3& v, double psi) {
    const double s = math::sin(psi), c = math::cos(psi);
    return {v.x * c - v.z * s, v.y, v.x * s + v.z * c};
}

constexpr double wrap_degrees(double a) {
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return a;
}

}  // namespace detail

// Magnetic field elements at geodetic position `p` and time `year` (decimal
// year; see decimal_year()). Equivalent to MAG_GeodeticToSpherical +
// MAG_TimelyModifyMagneticModel + MAG_Geomag + MAG_CalculateGridVariation.
//
// Grid variation is only defined here for |latitude| >= 55 degrees (polar
// stereographic); it is NaN elsewhere, as in the NCEI test values. The model's
// validity window (model.valid_from()..valid_until()) is not enforced.
template <int N>
constexpr MagneticElements magnetic_field(const Model<N>& model, const GeodeticCoord& p, double year,
                                          const Ellipsoid& e = wgs84) {
    const SphericalCoord s = to_spherical(p, e);
    const double phi = math::deg2rad(s.latitude_deg);
    const double lambda = math::deg2rad(s.longitude_deg);

    detail::Synthesis<N> sum{model, year - model.epoch};
    const double ratio = e.re / s.radius_km;
    sum.radius_power[0] = ratio * ratio;
    for (int n = 1; n <= N; ++n) sum.radius_power[n] = sum.radius_power[n - 1] * ratio;
    const double cos_l = math::cos(lambda), sin_l = math::sin(lambda);
    sum.cos_ml[0] = 1.0;
    sum.cos_ml[1] = cos_l;
    sum.sin_ml[1] = sin_l;
    for (int m = 2; m <= N; ++m) {
        sum.cos_ml[m] = sum.cos_ml[m - 1] * cos_l - sum.sin_ml[m - 1] * sin_l;
        sum.sin_ml[m] = sum.cos_ml[m - 1] * sin_l + sum.sin_ml[m - 1] * cos_l;
    }

    const double sin_phi = math::sin(phi);
    if (N <= 16 || 1.0 - math::abs(sin_phi) < 1.0e-10)
        sum.sum_low(sin_phi);
    else
        sum.sum_high(sin_phi);

    const double cos_phi = math::cos(phi);
    if (math::abs(cos_phi) > 1.0e-10) {
        sum.field.y /= cos_phi;
        sum.sv.y /= cos_phi;
    } else {
        sum.sum_pole_east(sin_phi);
    }

    const double psi = phi - math::deg2rad(p.latitude_deg);
    const detail::Vec3 b = detail::rotate(sum.field, psi);
    const detail::Vec3 db = detail::rotate(sum.sv, psi);

    // MAG_CalculateGeoMagneticElements / MAG_CalculateSecularVariationElements
    MagneticElements r;
    r.X = b.x;
    r.Y = b.y;
    r.Z = b.z;
    r.H = math::sqrt(b.x * b.x + b.y * b.y);
    r.F = math::sqrt(r.H * r.H + b.z * b.z);
    r.D = math::rad2deg(math::atan2(r.Y, r.X));
    r.I = math::rad2deg(math::atan2(r.Z, r.H));

    r.X_dot = db.x;
    r.Y_dot = db.y;
    r.Z_dot = db.z;
    r.H_dot = (r.X * r.X_dot + r.Y * r.Y_dot) / r.H;
    r.F_dot = (r.X * r.X_dot + r.Y * r.Y_dot + r.Z * r.Z_dot) / r.F;
    r.D_dot = math::rad2deg((r.X * r.Y_dot - r.Y * r.X_dot) / (r.H * r.H));
    r.I_dot = math::rad2deg((r.H * r.Z_dot - r.Z * r.H_dot) / (r.F * r.F));

    // MAG_CalculateGridVariation, polar stereographic branch only.
    if (p.latitude_deg >= 55.0)
        r.GV = detail::wrap_degrees(r.D - p.longitude_deg);
    else if (p.latitude_deg <= -55.0)
        r.GV = detail::wrap_degrees(r.D + p.longitude_deg);
    else
        r.GV = std::numeric_limits<double>::quiet_NaN();
    r.GV_dot = r.D_dot;
    return r;
}

// One-sigma model uncertainty (MAG_WMMHRErrorCalc). Fields that have no
// uncertainty estimate (GV and the rates) are zero.
constexpr MagneticElements wmmhr_uncertainty(const MagneticElements& at) {
    MagneticElements u;
    u.F = 134;
    u.H = 130;
    u.X = 135;
    u.Y = 85;
    u.Z = 134;
    u.I = 0.19;
    const double d_offset = 0.25, d_coef = 5205;
    const double d_var = d_coef / at.H;
    u.D = math::sqrt(d_offset * d_offset + d_var * d_var);
    if (u.D > 180) u.D = 180;
    return u;
}

}  // namespace wmm
