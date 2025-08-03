#ifndef HELPERS_H_
#define HELPERS_H_

#include <numbers>

// Helper for degree to radian conversion
template<typename T = float>
constexpr T degrees_to_radians(T degrees)
{
    return degrees * (std::numbers::pi_v<T> / 180.0f);
}

template<typename T = float>
constexpr T kph_to_mph(T kph)
{
    return kph * static_cast<T>(0.621371);
}

template<typename T = float>
constexpr T mph_to_kph(T mph)
{
    return mph * static_cast<T>(1.60934);
}

template<typename T = float>
constexpr T mph_to_mps(T mph)
{
    return mph * static_cast<T>(0.44704);
}

template<typename T = float>
constexpr T mps_to_mph(T mps)
{
    return mps * static_cast<T>(2.23694);
}

#endif // HELPERS_H_