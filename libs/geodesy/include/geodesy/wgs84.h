#pragma once

// The WGS 84 ellipsoid and its normal gravity field, from NIMA TR8350.2
// (3rd ed., amendment 2, 2004), tables 3.1-3.4.
//
// Only the four defining parameters (a, 1/f, GM, omega) are authoritative;
// the geometric constants below are derived from them rather than copied, so
// one mistyped digit cannot leave b and e^2 describing different ellipsoids.
// The normal-gravity constants ARE copied, because TR8350.2 publishes them to
// more digits than a derivation from the defining four reproduces in double,
// and wgs84_test checks the two agree to that precision.

namespace geodesy::wgs84 {

// Defining parameters.
inline constexpr double kA = 6378137.0;                  // semi-major axis, m
inline constexpr double kInvF = 298.257223563;           // inverse flattening
inline constexpr double kGM = 3.986004418e14;            // m^3/s^2, incl. atmosphere
inline constexpr double kOmegaIe = 7.292115e-5;          // earth rotation rate, rad/s

// Derived geometry.
inline constexpr double kF = 1.0 / kInvF;
inline constexpr double kB = kA * (1.0 - kF);            // semi-minor axis, m
inline constexpr double kE2 = kF * (2.0 - kF);           // first eccentricity squared
inline constexpr double kEp2 = kE2 / (1.0 - kE2);        // second eccentricity squared

// Normal gravity (Somigliana), TR8350.2 table 3.4.
inline constexpr double kGammaE = 9.7803253359;          // at the equator, m/s^2
inline constexpr double kGammaP = 9.8321849378;          // at the poles, m/s^2
inline constexpr double kSomiglianaK = 0.00193185265241;  // (b*gp)/(a*ge) - 1
inline constexpr double kM = 0.00344978650684;           // omega^2 a^2 b / GM

}  // namespace geodesy::wgs84
