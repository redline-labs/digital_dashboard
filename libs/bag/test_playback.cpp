// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playback key rules: `--remap old=new` and `--prefix`.
//
// Small, pure, and worth pinning because every way of getting it wrong is
// silent. A prefix or remap that produces an unpublishable key means those
// messages are dropped from the replay -- and a replay quietly missing a topic
// looks exactly like a recording that never contained it. Nothing errors,
// nothing warns, and the conclusion is "the recording is bad".
//
// This logic used to live inside the `play` verb, where none of it could be
// tested.

#include "bag/playback.h"

#include "pub_sub/topic_key.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void expectKey(const std::string& actual, const std::string& expected, const std::string& what)
{
    ++checks;
    if (actual != expected)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (got '%s', expected '%s')\n", what.c_str(), actual.c_str(),
                     expected.c_str());
    }
}

// ------------------------------------------------------------------- parsing

void testParsesWellFormedRemaps()
{
    std::vector<std::string> problems;
    const auto remaps = bag::parseRemaps({"a/one=b/one", "a/two=b/two"}, problems);

    expect(problems.empty(), "well-formed remaps produce no complaints");
    expect(remaps.size() == 2, "both are parsed");
    expect(remaps.at("a/one") == "b/one", "the first maps correctly");
    expect(remaps.at("a/two") == "b/two", "the second maps correctly");
}

// A value containing '=' is legitimate on the right-hand side; splitting on the
// LAST '=' instead of the first would mangle it.
void testSplitsOnTheFirstEquals()
{
    std::vector<std::string> problems;
    const auto remaps = bag::parseRemaps({"a=b=c"}, problems);

    expect(problems.empty(), "a value containing '=' is not a problem");
    expect(remaps.size() == 1 && remaps.at("a") == "b=c",
           "the split is on the FIRST '=', so the value keeps its own");
}

void testMalformedRemapsAreReported()
{
    {
        std::vector<std::string> problems;
        const auto remaps = bag::parseRemaps({"no-equals-here"}, problems);
        expect(problems.size() == 1, "a remap with no '=' is reported");
        expect(remaps.empty(), "and is not applied");
    }
    {
        std::vector<std::string> problems;
        bag::parseRemaps({"=b"}, problems);
        expect(problems.size() == 1, "an empty left-hand side is reported");
    }
    {
        std::vector<std::string> problems;
        bag::parseRemaps({"a="}, problems);
        expect(problems.size() == 1, "an empty right-hand side is reported");
    }
    {
        // Ambiguous: which one wins is not something a user should have to know.
        std::vector<std::string> problems;
        const auto remaps = bag::parseRemaps({"a=b", "a=c"}, problems);
        expect(problems.size() == 1, "a repeated source key is reported");
        expect(remaps.at("a") == "b", "and the first is kept rather than silently replaced");
    }
}

// One bad entry must not discard the good ones.
void testOneBadRemapDoesNotLoseTheRest()
{
    std::vector<std::string> problems;
    const auto remaps = bag::parseRemaps({"a=b", "broken", "c=d"}, problems);

    expect(problems.size() == 1, "only the broken one is reported");
    expect(remaps.size() == 2, "the other two survive");
    expect(remaps.count("a") == 1 && remaps.count("c") == 1, "and they are the right two");
}

// ---------------------------------------------------------------- resolution

void testNoRemapNoPrefixIsIdentity()
{
    expectKey(bag::resolvePlaybackKey("vehicle/engine/rpm", {}, ""), "vehicle/engine/rpm",
              "a key with nothing applied comes back unchanged");
}

void testPrefixIsPrepended()
{
    expectKey(bag::resolvePlaybackKey("vehicle/engine/rpm", {}, "replay"),
              "replay/vehicle/engine/rpm", "a prefix is prepended with a separator");
}

// The obvious thing to type, and it used to produce '//' -- an empty segment,
// which isValidTopicKey rejects, so EVERY message would have been dropped.
void testTrailingSlashesInThePrefixAreTrimmed()
{
    expectKey(bag::resolvePlaybackKey("a/b", {}, "replay/"), "replay/a/b",
              "a trailing '/' on the prefix does not produce an empty segment");
    expectKey(bag::resolvePlaybackKey("a/b", {}, "replay///"), "replay/a/b",
              "several trailing slashes are trimmed too");

    // The result has to be publishable, which is the whole point.
    expect(pub_sub::isValidTopicKey(bag::resolvePlaybackKey("a/b", {}, "replay/")),
           "and the result is a valid topic key");
}

void testPrefixOfOnlySlashesIsIgnored()
{
    expectKey(bag::resolvePlaybackKey("a/b", {}, "/"), "a/b",
              "a prefix of nothing but slashes is treated as no prefix");
}

void testRemapIsApplied()
{
    const std::map<std::string, std::string> remaps{{"vehicle/engine/rpm", "old/rpm"}};
    expectKey(bag::resolvePlaybackKey("vehicle/engine/rpm", remaps, ""), "old/rpm",
              "a remapped key is republished under its new name");
    expectKey(bag::resolvePlaybackKey("vehicle/speed_mps", remaps, ""), "vehicle/speed_mps",
              "and an unmapped key is untouched");
}

// THE ordering rule. A remap names a key AS RECORDED, so it has to be looked up
// before the prefix is added -- otherwise adding `--prefix` would silently stop
// every `--remap` from matching, and the user would see their remaps quietly
// stop working for no visible reason.
void testRemapIsAppliedBeforeThePrefix()
{
    const std::map<std::string, std::string> remaps{{"a/one", "b/one"}};

    expectKey(bag::resolvePlaybackKey("a/one", remaps, "replay"), "replay/b/one",
              "remap first, then prefix");

    // The failure this guards: if the prefix went on first, the lookup would be
    // for "replay/a/one", which is not in the map, and the remap would silently
    // do nothing.
    expect(bag::resolvePlaybackKey("a/one", remaps, "replay") != "replay/a/one",
           "the remap is NOT skipped when a prefix is also given");
}

// Whatever comes out has to be publishable, or the messages are dropped. These
// are the inputs a user would plausibly type.
void testResultsAreValidTopicKeys()
{
    const std::map<std::string, std::string> remaps{{"a/one", "b/one"}};

    for (const std::string prefix : {"", "replay", "replay/", "a/b", "a/b/"})
    {
        const std::string key = bag::resolvePlaybackKey("vehicle/engine/rpm", remaps, prefix);
        expect(pub_sub::isValidTopicKey(key),
               "prefix '" + prefix + "' yields a publishable key ('" + key + "')");
    }
}

// A remap TO an invalid key is the user's error, and resolution passes it
// through rather than silently correcting it -- the caller checks and reports.
// Asserted so nobody "helpfully" sanitises it here, which would turn a typo into
// a message published somewhere unexpected.
void testAnInvalidRemapTargetIsPassedThrough()
{
    const std::map<std::string, std::string> remaps{{"a/one", "bad@key"}};
    const std::string key = bag::resolvePlaybackKey("a/one", remaps, "");

    expectKey(key, "bad@key", "an invalid remap target is passed through unchanged");
    expect(!pub_sub::isValidTopicKey(key),
           "and it is detectably invalid, so the caller can refuse it");
}

}  // namespace

int main()
{
    testParsesWellFormedRemaps();
    testSplitsOnTheFirstEquals();
    testMalformedRemapsAreReported();
    testOneBadRemapDoesNotLoseTheRest();
    testNoRemapNoPrefixIsIdentity();
    testPrefixIsPrepended();
    testTrailingSlashesInThePrefixAreTrimmed();
    testPrefixOfOnlySlashesIsIgnored();
    testRemapIsApplied();
    testRemapIsAppliedBeforeThePrefix();
    testResultsAreValidTopicKeys();
    testAnInvalidRemapTargetIsPassedThrough();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
