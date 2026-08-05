#ifndef CONFIG_CODEC_CONFIG_LIMITS_H_
#define CONFIG_CODEC_CONFIG_LIMITS_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Range checking for widget configuration.
//
// A widget config comes straight from a YAML file, and until this existed every
// widget re-derived its own defensive logic -- mostly by not having any. The
// results were real: `max_rpm` was divided by without a zero check, a tick loop
// stepped `rpm += 100` towards a uint32_t bound and never terminated, min/max
// pairs arrived inverted and hit undefined behaviour in std::clamp, and a
// sparkline's update_rate of 2000 became `1000 / 2000 == 0` and repainted on
// every pass of the event loop.
//
// THE CONTRACT: a config struct may declare
//
//     std::vector<std::string> validate();
//
// It is called once, by widget_factory::createWidgetFromConfig, before the
// widget is constructed. It CLAMPS the config into a range the widget can
// actually draw and returns one message per adjustment, which the factory logs.
// It does not reject: a dashboard that comes up with one gauge showing a
// clamped range beats one that refuses to start on the way to a track day.
//
// Declaring it is optional -- a config with no ranges to check does not need
// one -- and the factory detects it, so nothing breaks by leaving it out.
namespace config_codec::limits {

// Ceilings chosen so a typo cannot turn into a hang. Each one is far above any
// plausible real value and far below the point where the drawing loops that
// consume them become expensive.
inline constexpr uint32_t kMaxRpmCeiling = 30000;
inline constexpr uint16_t kMaxUpdateRateHz = 240;
inline constexpr std::size_t kMaxMarkers = 64;

// Forces `value` into [lo, hi], appending a message if it had to move.
template <typename T>
void clampInto(T& value, T lo, T hi, const char* field, std::vector<std::string>& notes)
{
    const T before = value;
    value = std::clamp(value, lo, hi);
    if (value != before)
    {
        notes.push_back(std::string(field) + " was " + std::to_string(before) + ", clamped to " +
                        std::to_string(value));
    }
}

// A gauge's full-scale value. Zero is the dangerous one: it is the divisor in
// every "how far round the dial is this" calculation.
template <typename T>
void clampFullScale(T& value, const char* field, std::vector<std::string>& notes)
{
    clampInto<T>(value, T{1}, static_cast<T>(kMaxRpmCeiling), field, notes);
}

// Puts a min/max pair in order. std::clamp's precondition is !(max < min), so an
// inverted pair straight from a config file is undefined behaviour downstream.
template <typename T>
void orderRange(T& min_value, T& max_value, const char* field, std::vector<std::string>& notes)
{
    if (max_value < min_value)
    {
        std::swap(min_value, max_value);
        notes.push_back(std::string(field) + " had min above max; the two were swapped");
    }
    else if (max_value == min_value)
    {
        // A zero-width range makes the value-to-position division degenerate.
        // Widen rather than refuse, so the gauge draws something.
        max_value = min_value + T{1};
        notes.push_back(std::string(field) + " had min equal to max; max was raised by 1");
    }
}

// Caps a list's length. The speedometer's marker loop is O(n) per paint and its
// counter used to be the same width as the element type.
template <typename Container>
void capLength(Container& items, std::size_t limit, const char* field, std::vector<std::string>& notes)
{
    if (items.size() > limit)
    {
        notes.push_back(std::string(field) + " had " + std::to_string(items.size()) +
                        " entries, truncated to " + std::to_string(limit));
        items.resize(limit);
    }
}

}  // namespace config_codec::limits

#endif  // CONFIG_CODEC_CONFIG_LIMITS_H_
