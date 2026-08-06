// SPDX-License-Identifier: GPL-3.0-or-later
//
// Topic key validation and the mangling that packs a key into a liveliness
// token's key expression.
//
// This is small, pure, and worth testing carefully because every failure it
// guards against is silent. A '%' in a topic name does not crash anything: it
// mangles into a key that demangles into a *different* topic name, and the
// picker offers a signal that can never bind. A '@' does not crash anything
// either: the topic simply becomes invisible to every '**' subscriber in the
// tree while looking perfectly ordinary in the config file.
//
// The round trip over the shipped configs is the part that would actually
// catch a regression in practice -- it asserts the assumption the whole scheme
// rests on, against the real corpus, rather than against examples chosen to
// pass.

#include "pub_sub/topic_key.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

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

// ------------------------------------------------------------- validation

void testOrdinaryKeysAreValid()
{
    for (const char* key : {"vehicle/engine/rpm", "vehicle/speed_mps", "a", "a/b",
                            "vehicle/can0/rx", "Mixed/Case_1", "with-hyphen/ok"})
    {
        expect(pub_sub::isValidTopicKey(key), std::string("'") + key + "' is a valid topic key");
    }
}

void testStructuralProblemsAreRejected()
{
    expect(!pub_sub::isValidTopicKey(""), "an empty key is rejected");
    expect(!pub_sub::isValidTopicKey("/leading"), "a leading '/' is rejected");
    expect(!pub_sub::isValidTopicKey("trailing/"), "a trailing '/' is rejected");
    expect(!pub_sub::isValidTopicKey("a//b"), "an empty segment is rejected");
    expect(!pub_sub::isValidTopicKey("/"), "a lone '/' is rejected");
}

void testTheReservedCharactersAreRejected()
{
    // Each of these is a silent failure if it slips through, which is why they
    // are checked individually rather than as one "invalid character" case.
    expect(!pub_sub::isValidTopicKey("vehicle/engine%rpm"),
           "'%' is rejected -- it is the mangling separator, so a key with one "
           "could not be recovered from an advertisement");
    expect(!pub_sub::isValidTopicKey("@vehicle/engine"),
           "'@' is rejected -- a segment starting with it is verbatim in zenoh and "
           "no wildcard subscription would match the topic");
    expect(!pub_sub::isValidTopicKey("vehicle/@engine"),
           "'@' is rejected on an inner segment too, where it has the same effect");

    for (const char* key : {"bad*star", "bad$dollar", "bad?question", "bad#hash"})
    {
        expect(!pub_sub::isValidTopicKey(key),
               std::string("'") + key + "' is rejected -- zenoh does not allow the character");
    }

    for (const char* key : {"has space", "has.dot", "has:colon", "has+plus"})
    {
        expect(!pub_sub::isValidTopicKey(key),
               std::string("'") + key + "' is rejected by the allowlist");
    }
}

void testProblemsAreExplained()
{
    // The message is the whole value of the check: it has to say what to fix.
    expect(pub_sub::topicKeyProblem("vehicle/engine/rpm").empty(),
           "a valid key reports no problem");

    const std::string percent = pub_sub::topicKeyProblem("a%b");
    expect(percent.find('%') != std::string::npos,
           "the '%' message names the character");
    expect(percent.find("advertis") != std::string::npos,
           "the '%' message says why it matters, not just that it is invalid");

    const std::string at = pub_sub::topicKeyProblem("@a");
    expect(at.find("verbatim") != std::string::npos,
           "the '@' message explains the verbatim-segment behaviour");

    expect(pub_sub::topicKeyProblem("/a").find("starts with") != std::string::npos,
           "a leading slash gets its own message rather than a charset complaint");
    expect(pub_sub::topicKeyProblem("a/").find("ends with") != std::string::npos,
           "a trailing slash gets its own message");
    expect(pub_sub::topicKeyProblem("a//b").find("empty segment") != std::string::npos,
           "an empty segment gets its own message");
}

// ----------------------------------------------------- subscribe expressions

void testSubscribeExpressionsAllowWildcards()
{
    // The asymmetry that makes this a separate function: topic discovery
    // subscribes to "**", which is not a publishable topic key.
    expect(pub_sub::isValidSubscribeExpr("**"), "'**' is a valid subscription");
    expect(pub_sub::isValidSubscribeExpr("*"), "'*' is a valid subscription");
    expect(pub_sub::isValidSubscribeExpr("vehicle/**"), "a trailing '**' is valid");
    expect(pub_sub::isValidSubscribeExpr("vehicle/*/rpm"), "an inner '*' is valid");
    expect(pub_sub::isValidSubscribeExpr("vehicle/engine/rpm"),
           "a concrete key is also a valid subscription");

    expect(!pub_sub::isValidTopicKey("**"),
           "'**' is NOT a valid topic key -- a publisher cannot publish on a wildcard");
    expect(!pub_sub::isValidTopicKey("vehicle/**"), "a wildcard key is not publishable");

    expect(!pub_sub::isValidSubscribeExpr(""), "an empty subscription is rejected");
    expect(!pub_sub::isValidSubscribeExpr("/leading"), "a leading '/' is rejected");
    expect(!pub_sub::isValidSubscribeExpr("a//b"), "an empty segment is rejected");
    expect(!pub_sub::isValidSubscribeExpr("veh*icle/rpm"),
           "a partial-segment wildcard is rejected -- '*' is a whole segment or nothing");
}

// ------------------------------------------------------------- mangling

void testManglingRoundTrips()
{
    for (const char* key : {"vehicle/engine/rpm", "a", "a/b", "one/two/three/four/five",
                            "vehicle/can0/rx", "with-hyphen/and_underscore"})
    {
        const std::string mangled = pub_sub::mangleTopicKey(key);
        expect(mangled.find('/') == std::string::npos,
               std::string("'") + key + "' mangles to a single segment (no '/' left)");
        expect(pub_sub::demangleTopicKey(mangled) == key,
               std::string("'") + key + "' survives a mangle/demangle round trip");
    }
}

void testManglingIsASingleSegment()
{
    // The entire point: a topic has to occupy one segment or the advertisement
    // key cannot be parsed back, and wildcards on other fields stop working.
    const std::string mangled = pub_sub::mangleTopicKey("vehicle/engine/rpm");
    expect(mangled == "vehicle%engine%rpm", "mangling replaces every '/' with '%'");
}

// ---------------------------------------------------------- advertisement

void testAdvertiseKeyRoundTrips()
{
    const std::string key = pub_sub::advertiseKey("vehicle/engine/rpm", "EngineRpm");
    expect(key == "@redline/adv/EngineRpm/vehicle%engine%rpm",
           "the advertisement key has the expected shape");

    std::string topic;
    std::string schema;
    expect(pub_sub::parseAdvertiseKey(key, topic, schema), "it parses back");
    expect(topic == "vehicle/engine/rpm", "the topic survives");
    expect(schema == "EngineRpm", "the schema survives");
}

void testAdvertiseKeyIsInTheVerbatimSpace()
{
    // If this ever stops starting with '@', every '**' subscriber in the tree
    // starts seeing advertisements as if they were topics.
    const std::string key = pub_sub::advertiseKey("vehicle/engine/rpm", "EngineRpm");
    expect(!key.empty() && key.front() == '@',
           "the advertisement key starts with '@', keeping it out of '**' subscriptions");
    expect(std::string(pub_sub::kAdvertiseAll).front() == '@',
           "the discovery subscription is in the same verbatim space");
}

void testMalformedAdvertisementsAreRejected()
{
    std::string topic;
    std::string schema;

    // A discovery subscriber sees whatever is on the bus, including keys from a
    // build that does not exist yet. Skipping beats guessing.
    expect(!pub_sub::parseAdvertiseKey("", topic, schema), "an empty key is rejected");
    expect(!pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm", topic, schema),
           "a key missing the topic segment is rejected");
    expect(!pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm/topic/extra", topic, schema),
           "a longer form -- a newer build's -- is skipped rather than misparsed");
    expect(!pub_sub::parseAdvertiseKey("@other/adv/EngineRpm/topic", topic, schema),
           "another application's advertisement space is rejected");
    expect(!pub_sub::parseAdvertiseKey("vehicle/engine/rpm", topic, schema),
           "an ordinary topic key is not an advertisement");
    expect(!pub_sub::parseAdvertiseKey("@redline/adv//topic", topic, schema),
           "an empty schema segment is rejected");
}

// --------------------------------------------------- against the real corpus

void testEveryShippedKeyRoundTrips()
{
    // The assumption the whole scheme rests on, asserted against the actual
    // configs rather than against examples chosen to pass. If someone adds a
    // key with a '%' or a space, this fails here rather than silently in a
    // picker much later.
    const std::filesystem::path dir(REDLINE_CONFIG_DIR);
    if (!std::filesystem::is_directory(dir))
    {
        expect(false, "the config directory exists");
        return;
    }

    const std::regex pattern(R"(zenoh_key:\s*\"?([^\"\n]*?)\"?\s*$)");
    std::set<std::string> keys;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (entry.path().extension() != ".yaml")
        {
            continue;
        }

        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line))
        {
            std::smatch match;
            if (std::regex_search(line, match, pattern) && !match[1].str().empty())
            {
                keys.insert(match[1].str());
            }
        }
    }

    expect(!keys.empty(), "at least one zenoh_key was found in the shipped configs");

    for (const std::string& key : keys)
    {
        const std::string problem = pub_sub::topicKeyProblem(key);
        expect(problem.empty(), "shipped key '" + key + "' is valid (" + problem + ")");
        expect(pub_sub::demangleTopicKey(pub_sub::mangleTopicKey(key)) == key,
               "shipped key '" + key + "' survives a mangle round trip");

        std::string topic;
        std::string schema;
        expect(pub_sub::parseAdvertiseKey(pub_sub::advertiseKey(key, "EngineRpm"), topic, schema) &&
                   topic == key,
               "shipped key '" + key + "' survives an advertise/parse round trip");
    }

    std::fprintf(stderr, "  (checked %zu distinct shipped keys)\n", keys.size());
}

}  // namespace

int main()
{
    testOrdinaryKeysAreValid();
    testStructuralProblemsAreRejected();
    testTheReservedCharactersAreRejected();
    testProblemsAreExplained();

    testSubscribeExpressionsAllowWildcards();

    testManglingRoundTrips();
    testManglingIsASingleSegment();

    testAdvertiseKeyRoundTrips();
    testAdvertiseKeyIsInTheVerbatimSpace();
    testMalformedAdvertisementsAreRejected();

    testEveryShippedKeyRoundTrips();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
