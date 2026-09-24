#pragma once

// The International Standard Atmosphere's troposphere, for the barometer:
// pressure altitude from static pressure and back. What it gets wrong at a
// given place and day -- the weather, the geoid, a warm afternoon -- is an
// offset the estimator learns (baro_offset), not a model this tries to fix.

#include <cmath>

namespace vehicle_estimator::isa
{

inline constexpr double kP0 = 101325.0;      // Pa, sea-level standard pressure
inline constexpr double kT0 = 288.15;        // K
inline constexpr double kLapse = 0.0065;     // K/m
inline constexpr double kG0 = 9.80665;       // m/s^2
inline constexpr double kMolar = 0.0289644;  // kg/mol, dry air
inline constexpr double kGas = 8.3144598;    // J/(mol K)
// g M / (R L), the exponent of the pressure-height relation.
inline constexpr double kExponent = kG0 * kMolar / (kGas * kLapse);

// Pressure altitude, m, from static pressure, Pa.
inline double altitude(double pa)
{
    return kT0 / kLapse * (1.0 - std::pow(pa / kP0, 1.0 / kExponent));
}

// Static pressure, Pa, at pressure altitude h, m.
inline double pressure(double h)
{
    return kP0 * std::pow(1.0 - kLapse * h / kT0, kExponent);
}

// Air density, kg/m^3, at pressure altitude h.
inline double density(double h)
{
    return pressure(h) * kMolar / (kGas * (kT0 - kLapse * h));
}

}  // namespace vehicle_estimator::isa
