#ifndef CARPLAY_NAV_FORMAT_H_
#define CARPLAY_NAV_FORMAT_H_

#include <cmath>
#include <cstdint>
#include <string>

// Distance/time/ETA formatting for the navigation widget.
//
// Split out of the widget and kept free of Qt on purpose: this is the only part
// of the widget that is not about pixels, it is where the rounding rules and the
// unit thresholds live, and it is the part worth a unit test. The widget itself
// is verified by looking at it.
namespace carplay_nav {

// Distances arrive from iAP2 in metres, always.
inline constexpr float kMetresPerFoot = 0.3048f;
inline constexpr float kMetresPerMile = 1609.344f;

// Below this, a distance reads in the small unit (metres/feet) rounded to a
// coarse step; above it, in the large one (km/miles) with one decimal.
inline constexpr float kMetricSmallUnitCutoffM = 1000.0f;
inline constexpr float kImperialSmallUnitCutoffM = 0.1f * kMetresPerMile;

// Rounds to the nearest `step`, staying in float. Turn distances count down
// continuously and a raw value flickers the last digit every frame; the real
// thing steps.
inline float roundToStep(float value, float step)
{
    if (!(step > 0.0f) || !std::isfinite(value))
    {
        return value;
    }
    return std::round(value / step) * step;
}

// Renders `value` with `decimals` digits after the point, without <format> or
// snprintf round-trips. `decimals` is 0 or 1 here.
inline std::string fixed(float value, int decimals)
{
    if (!std::isfinite(value))
    {
        value = 0.0f;
    }
    if (decimals <= 0)
    {
        return std::to_string(static_cast<long long>(std::llround(value)));
    }

    const long long scaled = std::llround(std::fabs(value) * 10.0);
    const std::string sign = value < 0.0f ? "-" : "";
    return sign + std::to_string(scaled / 10) + "." + std::to_string(scaled % 10);
}

// "400 m" / "1.2 km", or "300 ft" / "0.4 mi".
//
// The step sizes are what a turn card actually shows: coarse when the turn is
// far away, finer as it closes, and never a distance more precise than the
// position fix justifies.
inline std::string formatDistance(float metres, bool imperial)
{
    if (!std::isfinite(metres) || metres < 0.0f)
    {
        metres = 0.0f;
    }

    if (imperial)
    {
        if (metres < kImperialSmallUnitCutoffM)
        {
            const float feet = metres / kMetresPerFoot;
            const float step = feet < 500.0f ? 50.0f : 100.0f;
            return fixed(roundToStep(feet, step), 0) + " ft";
        }
        return fixed(metres / kMetresPerMile, 1) + " mi";
    }

    if (metres < kMetricSmallUnitCutoffM)
    {
        const float step = metres < 100.0f ? 10.0f : 50.0f;
        return fixed(roundToStep(metres, step), 0) + " m";
    }
    return fixed(metres / 1000.0f, 1) + " km";
}

// "8 min" / "1 hr 5 min". Anything under a minute is still "1 min": a turn card
// that says "0 min" reads as broken rather than as nearly there.
inline std::string formatDuration(float seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0f)
    {
        seconds = 0.0f;
    }

    const long long total_minutes = std::llround(seconds / 60.0);
    const long long minutes = total_minutes < 1 ? 1 : total_minutes;

    if (minutes < 60)
    {
        return std::to_string(minutes) + " min";
    }

    const long long hours = minutes / 60;
    const long long remainder = minutes % 60;
    if (remainder == 0)
    {
        return std::to_string(hours) + " hr";
    }
    return std::to_string(hours) + " hr " + std::to_string(remainder) + " min";
}

// Which glyph the maneuver arrow should be.
//
// Deliberately derived from the turn ANGLE rather than from iAP2's maneuverType
// enumeration. The angle is unambiguous -- degrees, signed, clockwise-positive --
// whereas the maneuver-type constants are Apple's and this tree has no
// authoritative copy of them. Guessing at that table would produce arrows that
// are confidently wrong, which on a turn card is worse than a plain one.
// maneuverType is still carried on the wire and shown as a debug field.
enum class ManeuverGlyph
{
    Straight,
    SlightLeft,
    Left,
    SharpLeft,
    SlightRight,
    Right,
    SharpRight,
    UTurn
};

// Thresholds in degrees away from straight ahead.
inline constexpr float kSlightTurnDeg = 20.0f;
inline constexpr float kSharpTurnDeg = 115.0f;
inline constexpr float kUTurnDeg = 160.0f;

inline ManeuverGlyph glyphForAngle(float angle_deg)
{
    if (!std::isfinite(angle_deg))
    {
        return ManeuverGlyph::Straight;
    }

    // Normalise into (-180, 180] so a publisher using 0..360 still lands in the
    // right bucket rather than always reading as a hard right.
    while (angle_deg > 180.0f)
    {
        angle_deg -= 360.0f;
    }
    while (angle_deg <= -180.0f)
    {
        angle_deg += 360.0f;
    }

    const float magnitude = std::fabs(angle_deg);
    if (magnitude >= kUTurnDeg)
    {
        return ManeuverGlyph::UTurn;
    }
    if (magnitude < kSlightTurnDeg)
    {
        return ManeuverGlyph::Straight;
    }

    const bool right = angle_deg > 0.0f;
    if (magnitude >= kSharpTurnDeg)
    {
        return right ? ManeuverGlyph::SharpRight : ManeuverGlyph::SharpLeft;
    }
    if (magnitude < 55.0f)
    {
        return right ? ManeuverGlyph::SlightRight : ManeuverGlyph::SlightLeft;
    }
    return right ? ManeuverGlyph::Right : ManeuverGlyph::Left;
}

}  // namespace carplay_nav

#endif  // CARPLAY_NAV_FORMAT_H_
