#ifndef DASHBOARD_WIDGET_LIMITS_H_
#define DASHBOARD_WIDGET_LIMITS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config_codec/config_limits.h"

// The ceilings a widget's validate() clamps to. Each is far above any
// plausible real value and far below the point where the drawing loops that
// consume it become expensive, so a typo cannot turn into a hang.
namespace dashboard::limits
{

inline constexpr uint32_t kMaxRpmCeiling = 30000;
inline constexpr uint16_t kMaxUpdateRateHz = 240;
inline constexpr std::size_t kMaxMarkers = 64;

// A gauge's full-scale value. Zero is the dangerous one: it is the divisor in
// every "how far round the dial is this" calculation.
template <typename T>
void clampFullScale(T& value, const char* field, std::vector<std::string>& notes)
{
    config_codec::limits::clampInto<T>(value, T{1}, static_cast<T>(kMaxRpmCeiling), field, notes);
}

}  // namespace dashboard::limits

#endif  // DASHBOARD_WIDGET_LIMITS_H_
