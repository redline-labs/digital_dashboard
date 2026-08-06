// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wildcard matcher `inspect list -k` filters with.
//
// Worth testing because its failure is invisible in the worst way: a filter that
// wrongly excludes a topic produces a shorter list, and a shorter list looks
// exactly like a bus with fewer publishers. Nothing errors, nothing warns, and
// the user concludes their node is not running.
//
// It also exists precisely because it is NOT zenoh's own matcher (see
// inspect/key_match.h -- using zenoh's would mean a KeyExpr construction per row
// and <zenoh.hxx> in the verb). Anything reimplemented rather than reused has to
// earn that with a test.

#include "inspect/key_match.h"

#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expectMatch(std::string_view pattern, std::string_view key)
{
    ++checks;
    if (!inspect::keyMatches(pattern, key))
    {
        ++failures;
        std::fprintf(stderr, "FAIL: '%s' should match '%s'\n", std::string(pattern).c_str(),
                     std::string(key).c_str());
    }
}

void expectNoMatch(std::string_view pattern, std::string_view key)
{
    ++checks;
    if (inspect::keyMatches(pattern, key))
    {
        ++failures;
        std::fprintf(stderr, "FAIL: '%s' should NOT match '%s'\n", std::string(pattern).c_str(),
                     std::string(key).c_str());
    }
}

void testExact()
{
    expectMatch("vehicle/engine/rpm", "vehicle/engine/rpm");
    expectNoMatch("vehicle/engine/rpm", "vehicle/engine/rp");
    expectNoMatch("vehicle/engine/rpm", "vehicle/engine/rpmx");
    expectNoMatch("vehicle/engine", "vehicle/engine/rpm");
    expectNoMatch("vehicle/engine/rpm", "vehicle/engine");
}

// The default filter. Everything must match it, or `inspect list` with no
// arguments shows nothing -- which would look exactly like an empty bus.
void testMatchAll()
{
    expectMatch("**", "a");
    expectMatch("**", "vehicle/engine/rpm");
    expectMatch("**", "a/b/c/d/e/f/g");
}

// '*' is one segment, not "any characters". Getting this wrong makes
// 'vehicle/*' match 'vehicle/engine/rpm', which silently widens every filter.
void testSingleSegmentWildcard()
{
    expectMatch("vehicle/*", "vehicle/speed_mps");
    expectNoMatch("vehicle/*", "vehicle/engine/rpm");
    expectNoMatch("vehicle/*", "vehicle");

    expectMatch("*/engine/rpm", "vehicle/engine/rpm");
    expectNoMatch("*/engine/rpm", "a/b/engine/rpm");

    expectMatch("vehicle/*/rpm", "vehicle/engine/rpm");
    expectNoMatch("vehicle/*/rpm", "vehicle/engine/inner/rpm");

    expectMatch("*", "vehicle");
    expectNoMatch("*", "vehicle/speed");
}

// '**' is ZERO or more segments. The zero case is the one an implementation
// tends to get wrong, and it is not academic: 'vehicle/**' must list
// 'vehicle' itself if such a topic exists.
void testMultiSegmentWildcard()
{
    expectMatch("vehicle/**", "vehicle/engine/rpm");
    expectMatch("vehicle/**", "vehicle/speed_mps");
    expectMatch("vehicle/**", "vehicle");
    expectNoMatch("vehicle/**", "nodes/carplay/video");

    expectMatch("**/rpm", "vehicle/engine/rpm");
    expectMatch("**/rpm", "rpm");
    expectNoMatch("**/rpm", "vehicle/engine/temperature");

    // The zero-segment case in the middle.
    expectMatch("a/**/b", "a/b");
    expectMatch("a/**/b", "a/x/b");
    expectMatch("a/**/b", "a/x/y/z/b");
    expectNoMatch("a/**/b", "a/x/y/z");

    // Several '**' in one pattern.
    expectMatch("**/engine/**", "vehicle/engine/rpm");
    expectMatch("**/engine/**", "engine/rpm");
    expectMatch("**/engine/**", "engine");
}

void testMixedWildcards()
{
    expectMatch("vehicle/*/**", "vehicle/engine/rpm");
    expectMatch("vehicle/*/**", "vehicle/engine");
    expectNoMatch("vehicle/*/**", "vehicle");

    expectMatch("**/*/rpm", "vehicle/engine/rpm");
    expectNoMatch("**/*/rpm", "rpm");
}

// The keys this tree actually publishes, so the matcher is exercised against
// real shapes rather than only against cases invented to pass.
void testAgainstRealKeys()
{
    const char* keys[] = {
        "vehicle/engine/rpm",
        "vehicle/engine/temperature_celsius",
        "vehicle/odometer",
        "vehicle/speed_mps",
        "vehicle/telltales/battery_warning",
        "vehicle/can0/rx",
        "nodes/carplay/video",
        "nodes/motec_m1/engine_air",
        "nodes/racegrade_tc8/inputs",
    };

    for (const char* key : keys)
    {
        expectMatch("**", key);
    }

    expectMatch("vehicle/**", "vehicle/telltales/battery_warning");
    expectNoMatch("vehicle/**", "nodes/carplay/video");

    expectMatch("nodes/**", "nodes/motec_m1/engine_air");
    expectNoMatch("nodes/**", "vehicle/odometer");

    expectMatch("nodes/*/inputs", "nodes/racegrade_tc8/inputs");
    expectNoMatch("nodes/*/inputs", "nodes/motec_m1/engine_air");

    expectMatch("**/battery_warning", "vehicle/telltales/battery_warning");
}

// A pattern with an empty segment, or an empty key. Neither is a valid topic
// key, but a filter comes from a command line and can be anything.
void testDegenerate()
{
    expectMatch("", "");
    expectNoMatch("", "a");
    expectNoMatch("a", "");
    expectMatch("**", "");
}

}  // namespace

int main()
{
    testExact();
    testMatchAll();
    testSingleSegmentWildcard();
    testMultiSegmentWildcard();
    testMixedWildcards();
    testAgainstRealKeys();
    testDegenerate();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
