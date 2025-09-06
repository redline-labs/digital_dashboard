#ifndef UNIT_CONVERSION_H_
#define UNIT_CONVERSION_H_

#include <numbers>

// Helper for degree to radian conversion
template<typename T = float>
constexpr T degrees_to_radians(T degrees)
{
    return degrees * (std::numbers::pi_v<T> / static_cast<T>(180.0));
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

template<typename T = float>
constexpr T psi_to_bar(T psi)
{
    return psi * static_cast<T>(0.0689476);
}

template<typename T = float>
constexpr T bar_to_psi(T bar)
{
    return bar * static_cast<T>(14.5037738007218);
}

template<typename T = float>
constexpr T celsius_to_fahrenheit(T celsius)
{
    return celsius * static_cast<T>(1.8) + static_cast<T>(32);
}

template<typename T = float>
constexpr T fahrenheit_to_celsius(T fahrenheit)
{
    return (fahrenheit - static_cast<T>(32)) / static_cast<T>(1.8);
}



#endif // UNIT_CONVERSION_H_
