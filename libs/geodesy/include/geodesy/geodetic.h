#pragma once

// Geodetic <-> ECEF and the local NED frame, on the WGS 84 ellipsoid.
//
// Every function is a template over the scalar and branch-free, so the same
// code runs on double at runtime, in a static_assert, and on csym::Expr inside
// a factor that csym differentiates. That is why the math goes through csym's
// front-ends (constexpr during constant evaluation, <cmath> otherwise) and why
// ecefToLlh is Vermeille's closed form rather than the usual iteration: a
// loop that stops on a tolerance cannot be traced.
//
// Angles are radians throughout. A plausible-looking wrong latitude is the
// failure mode here, so the checked entry points at the bottom are what code
// holding a measurement should call.

#include <cmath>

#include "csym/math/cmath.h"
#include "csym/matrix.h"
#include "geodesy/wgs84.h"

namespace geodesy {

template <class T>
struct Llh {
    T lat{0};  // geodetic latitude, rad
    T lon{0};  // longitude, rad, east positive
    T h{0};    // height above the ellipsoid, m
};

// Prime-vertical radius of curvature N(lat).
template <class T>
constexpr T primeVerticalRadius(const T& lat) {
    const T s = csym::sin(lat);
    return T(wgs84::kA) / csym::sqrt(T(1) - T(wgs84::kE2) * s * s);
}

// Meridian radius of curvature M(lat).
template <class T>
constexpr T meridianRadius(const T& lat) {
    const T s = csym::sin(lat);
    const T d = T(1) - T(wgs84::kE2) * s * s;
    return T(wgs84::kA * (1.0 - wgs84::kE2)) / (d * csym::sqrt(d));
}

template <class T>
constexpr csym::Vector3<T> llhToEcef(const Llh<T>& p) {
    const T sl = csym::sin(p.lat), cl = csym::cos(p.lat);
    const T n = primeVerticalRadius(p.lat);
    return csym::Vector3<T>{(n + p.h) * cl * csym::cos(p.lon),
                            (n + p.h) * cl * csym::sin(p.lon),
                            (n * T(1.0 - wgs84::kE2) + p.h) * sl};
}

// Vermeille (2011), "An analytical method to transform geocentric into
// geodetic coordinates", J. Geodesy 85:105-117. Exact (no iteration) for
// every point outside the ellipsoid's evolute, a region within ~43 km of the
// earth's centre.
template <class T>
constexpr Llh<T> ecefToLlh(const csym::Vector3<T>& e) {
    constexpr double a2 = wgs84::kA * wgs84::kA;
    constexpr double e2 = wgs84::kE2;
    constexpr double e4 = e2 * e2;
    const T x = e[0], y = e[1], z = e[2];
    const T rho2 = x * x + y * y;
    const T rho = csym::sqrt(rho2);
    const T p = rho2 / T(a2);
    const T q = T((1.0 - e2) / a2) * z * z;
    const T r = (p + q - T(e4)) / T(6);
    const T s = T(e4 / 4.0) * p * q / (r * r * r);
    const T t = csym::pow(T(1) + s + csym::sqrt(s * (T(2) + s)), T(1.0 / 3.0));
    const T u = r * (T(1) + t + T(1) / t);
    const T v = csym::sqrt(u * u + T(e4) * q);
    const T w = T(e2) * (u + v - q) / (T(2) * v);
    const T k = csym::sqrt(u + v + w * w) - w;
    const T d = k * rho / (k + T(e2));
    const T dz = csym::sqrt(d * d + z * z);
    return Llh<T>{T(2) * csym::atan2(z, d + dz), csym::atan2(y, x), (k + T(e2) - T(1)) / k * dz};
}

// The NED axes expressed in ECEF, as columns: v_e = R_e_n * v_n.
template <class T>
constexpr csym::Matrix33<T> rotEcefFromNed(const T& lat, const T& lon) {
    const T sl = csym::sin(lat), cl = csym::cos(lat);
    const T so = csym::sin(lon), co = csym::cos(lon);
    //                     north        east   down
    return csym::Matrix33<T>{-sl * co, -so, -cl * co,  //
                             -sl * so, co, -cl * so,   //
                             cl, T(0), -sl};
}

// ---- checked entry points -------------------------------------------------

// A position a receiver could have reported: finite, latitude within the
// poles, and within 100 km of the ellipsoid (well clear of the evolute where
// ecefToLlh stops being exact, and of anything a car does).
inline constexpr double kMaxAbsHeightM = 100e3;

constexpr bool isPlausible(const Llh<double>& p) {
    const auto finite = [](double v) { return v == v && v - v == 0.0; };  // not NaN, not inf
    return finite(p.lat) && finite(p.lon) && finite(p.h) && p.lat >= -1.5707963267948966 &&
           p.lat <= 1.5707963267948966 && p.h >= -kMaxAbsHeightM && p.h <= kMaxAbsHeightM;
}

constexpr bool isPlausibleEcef(const csym::Vector3<double>& e) {
    for (std::size_t i = 0; i < 3; ++i)
        if (!(e[i] == e[i] && e[i] - e[i] == 0.0)) return false;
    const double r = csym::sqrt(e.squared_norm());
    return r >= wgs84::kB - kMaxAbsHeightM && r <= wgs84::kA + kMaxAbsHeightM;
}

}  // namespace geodesy
