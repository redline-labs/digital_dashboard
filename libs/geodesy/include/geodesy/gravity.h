#pragma once

// Gravity: the vector an accelerometer at rest in the ECEF frame reads the
// negative of. Gravitation plus the centrifugal term of the earth's rotation,
// which is what an ECEF-frame navigation equation wants.
//
// normalGravity* is the WGS 84 ellipsoid's own field: Somigliana at the
// surface, TR8350.2's second-order height expansion above it, and the small
// north component that appears off the surface. It is not the real field.
// The real field differs by up to ~0.1 mGal-scale anomalies and by the
// deflection of the vertical (the plumb line leaning up to tens of arcseconds
// off the ellipsoid normal), which is what an EGM2008 or DEFLEC model adds.
// GravityModel is the seam for that: the estimator holds one and never knows
// which.

#include "csym/math/cmath.h"
#include "csym/matrix.h"
#include "geodesy/geodetic.h"
#include "geodesy/wgs84.h"

namespace geodesy {

// |gamma| on the ellipsoid surface, m/s^2 (Somigliana's closed form).
template <class T>
constexpr T normalGravitySurface(const T& lat) {
    const T s2 = csym::sin(lat) * csym::sin(lat);
    return T(wgs84::kGammaE) * (T(1) + T(wgs84::kSomiglianaK) * s2) /
           csym::sqrt(T(1) - T(wgs84::kE2) * s2);
}

// Normal gravity in NED at height h. The down component is TR8350.2 eq. 4-3;
// the north component is the first-order term from Groves (2013) eq. 2.140,
// -8.08e-9 * h * sin(2 lat), a few micro-g at track altitudes -- here because
// leaving it out is an approximation the rest of the model does not make.
template <class T>
constexpr csym::Vector3<T> normalGravityNed(const T& lat, const T& h) {
    constexpr double a = wgs84::kA, f = wgs84::kF, m = wgs84::kM;
    const T s = csym::sin(lat);
    const T g0 = normalGravitySurface(lat);
    const T down = g0 * (T(1) - T(2.0 / a) * (T(1.0 + f + m) - T(2.0 * f) * s * s) * h + T(3.0 / (a * a)) * h * h);
    const T north = T(-8.08e-9) * h * csym::sin(T(2) * lat);
    return csym::Vector3<T>{north, T(0), down};
}

template <class T>
constexpr csym::Vector3<T> normalGravityEcef(const csym::Vector3<T>& p_e) {
    const Llh<T> llh = ecefToLlh(p_e);
    return rotEcefFromNed(llh.lat, llh.lon) * normalGravityNed(llh.lat, llh.h);
}

// The earth's rotation, in ECEF.
inline constexpr csym::Vector3<double> kOmegaIeEcef{0.0, 0.0, wgs84::kOmegaIe};

// Where gravity comes from, at runtime. Evaluated at a position estimate and
// handed to a factor as a constant: over one IMU interval the change in
// gravity with the position error is ~3e-6 s^-2 per metre, far below
// anything the accelerometer resolves, so it does not need a Jacobian.
class GravityModel {
  public:
    virtual ~GravityModel() = default;
    virtual csym::Vector3<double> gravityEcef(const csym::Vector3<double>& p_e) const = 0;
};

class NormalGravity final : public GravityModel {
  public:
    csym::Vector3<double> gravityEcef(const csym::Vector3<double>& p_e) const override {
        return normalGravityEcef(p_e);
    }
};

}  // namespace geodesy
