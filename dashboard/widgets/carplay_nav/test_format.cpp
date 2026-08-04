// What the turn card says about a distance, a duration and a turn angle.
//
// These are the rules a driver reads at a glance, so the assertions are about
// the exact rendered string and the exact bucket, not just "it did not crash".
// The bad-input cases are the point: route guidance arrives over a radio link
// from a phone, and a dropped field shows up here as 0, NaN or a negative.

#include "carplay_nav/format.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void checkEq(const std::string& actual, const std::string& expected, const std::string& what)
{
    ++g_checks;
    if (actual != expected)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s (got '%s', wanted '%s')\n", what.c_str(), actual.c_str(),
                     expected.c_str());
    }
}

using namespace carplay_nav;

void testMetricDistances()
{
    checkEq(formatDistance(0.0f, false), "0 m", "zero metres");
    checkEq(formatDistance(42.0f, false), "40 m", "close distances step by 10");
    checkEq(formatDistance(47.0f, false), "50 m", "close distances round to the nearest 10");
    checkEq(formatDistance(420.0f, false), "400 m", "mid distances step by 50");
    checkEq(formatDistance(430.0f, false), "450 m", "mid distances round to the nearest 50");
    checkEq(formatDistance(999.0f, false), "1000 m", "just under the cutoff stays in metres");
    checkEq(formatDistance(1000.0f, false), "1.0 km", "at the cutoff it switches to km");
    checkEq(formatDistance(12345.0f, false), "12.3 km", "km carry one decimal");
}

void testImperialDistances()
{
    checkEq(formatDistance(0.0f, true), "0 ft", "zero feet");
    // 30 m is a little under 100 ft, which rounds to the nearest 50.
    checkEq(formatDistance(30.0f, true), "100 ft", "close distances step by 50 feet");
    checkEq(formatDistance(kMetresPerMile, true), "1.0 mi", "a mile reads as 1.0 mi");
    checkEq(formatDistance(kMetresPerMile * 2.5f, true), "2.5 mi", "miles carry one decimal");
}

void testDistancesSurviveBadInput()
{
    // A dropped field arrives as 0; a bad division upstream arrives as NaN or
    // inf. None of them may reach the painter as text like "nan m".
    checkEq(formatDistance(-500.0f, false), "0 m", "a negative distance clamps to zero");
    checkEq(formatDistance(std::numeric_limits<float>::quiet_NaN(), false), "0 m",
            "a NaN distance clamps to zero");
    checkEq(formatDistance(std::numeric_limits<float>::infinity(), false), "0 m",
            "an infinite distance clamps to zero");
    checkEq(formatDistance(std::numeric_limits<float>::quiet_NaN(), true), "0 ft",
            "a NaN distance clamps to zero in imperial too");
}

void testDurations()
{
    checkEq(formatDuration(0.0f), "1 min", "under a minute still reads as 1 min, never 0");
    checkEq(formatDuration(20.0f), "1 min", "a part-minute rounds up rather than to zero");
    checkEq(formatDuration(480.0f), "8 min", "whole minutes");
    checkEq(formatDuration(3600.0f), "1 hr", "an exact hour drops the minutes");
    checkEq(formatDuration(3900.0f), "1 hr 5 min", "hours and minutes");
    checkEq(formatDuration(-90.0f), "1 min", "a negative duration clamps to the floor");
    checkEq(formatDuration(std::numeric_limits<float>::quiet_NaN()), "1 min",
            "a NaN duration clamps to the floor");
}

void testGlyphBuckets()
{
    check(glyphForAngle(0.0f) == ManeuverGlyph::Straight, "0 degrees is straight ahead");
    check(glyphForAngle(10.0f) == ManeuverGlyph::Straight, "a small angle is still straight");
    check(glyphForAngle(40.0f) == ManeuverGlyph::SlightRight, "40 degrees is a slight right");
    check(glyphForAngle(-40.0f) == ManeuverGlyph::SlightLeft, "-40 degrees is a slight left");
    check(glyphForAngle(90.0f) == ManeuverGlyph::Right, "90 degrees is a right");
    check(glyphForAngle(-90.0f) == ManeuverGlyph::Left, "-90 degrees is a left");
    check(glyphForAngle(130.0f) == ManeuverGlyph::SharpRight, "130 degrees is a sharp right");
    check(glyphForAngle(-130.0f) == ManeuverGlyph::SharpLeft, "-130 degrees is a sharp left");
    check(glyphForAngle(175.0f) == ManeuverGlyph::UTurn, "175 degrees is a U-turn");
}

void testGlyphNormalisesTheAngle()
{
    // A publisher reporting 0..360 rather than -180..180 would otherwise have
    // every left turn come out as a hard right.
    check(glyphForAngle(270.0f) == ManeuverGlyph::Left,
          "270 degrees normalises to -90 and reads as a left");
    check(glyphForAngle(320.0f) == ManeuverGlyph::SlightLeft,
          "320 degrees normalises to -40 and reads as a slight left");
    check(glyphForAngle(360.0f) == ManeuverGlyph::Straight,
          "360 degrees normalises to 0 and reads as straight");
    check(glyphForAngle(-270.0f) == ManeuverGlyph::Right,
          "-270 degrees normalises to 90 and reads as a right");

    check(glyphForAngle(std::numeric_limits<float>::quiet_NaN()) == ManeuverGlyph::Straight,
          "a NaN angle falls back to straight rather than looping forever");
}

void testRoundToStep()
{
    check(roundToStep(47.0f, 10.0f) == 50.0f, "rounds up to the nearest step");
    check(roundToStep(44.0f, 10.0f) == 40.0f, "rounds down to the nearest step");
    // A zero or negative step would divide by zero and hand a NaN to the text.
    check(roundToStep(47.0f, 0.0f) == 47.0f, "a zero step is a no-op rather than a division");
    check(roundToStep(47.0f, -10.0f) == 47.0f, "a negative step is a no-op");
}

}  // namespace

int main()
{
    testMetricDistances();
    testImperialDistances();
    testDistancesSurviveBadInput();
    testDurations();
    testGlyphBuckets();
    testGlyphNormalisesTheAngle();
    testRoundToStep();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
