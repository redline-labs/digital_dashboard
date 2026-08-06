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
    expect(!pub_sub::parseAdvertiseKey("@other/adv/EngineRpm/topic", topic, schema),
           "another application's advertisement space is rejected");
    expect(!pub_sub::parseAdvertiseKey("vehicle/engine/rpm", topic, schema),
           "an ordinary topic key is not an advertisement");
    expect(!pub_sub::parseAdvertiseKey("@redline/adv//topic", topic, schema),
           "an empty schema segment is rejected");
}

// The extensibility rule, and the reason it is a test rather than a convention.
//
// parseAdvertiseKey used to require EXACTLY four segments. TopicDirectory drops
// every key it rejects, so the first build to append a fifth would have emptied
// the topic picker of every build that predates it -- silently, because a picker
// that parses no advertisements looks exactly like a bus with no publishers.
//
// Mutation-check: change kMinimumSegments back to an equality test in
// topic_key.cpp and testExtraSegmentsAreTolerated must fail.
void testExtraSegmentsAreTolerated()
{
    std::string topic;
    std::string schema;

    expect(pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm/vehicle%engine%rpm/abc123", topic,
                                      schema),
           "a five-segment advertisement -- a newer build's -- still parses");
    expect(topic == "vehicle/engine/rpm" && schema == "EngineRpm",
           "and the segments this build understands are read correctly");

    expect(pub_sub::parseAdvertiseKey(
               "@redline/adv/EngineRpm/vehicle%engine%rpm/abc123/something/else", topic, schema),
           "segments beyond the fifth are ignored rather than fatal");
    expect(topic == "vehicle/engine/rpm" && schema == "EngineRpm",
           "and the known segments are still read correctly");
}

// The zid overload. Empty means "this advertiser did not say", which is what an
// older publisher looks like -- and is never to be reported as "no owner".
void testZidOverload()
{
    std::string topic;
    std::string schema;
    std::string zid;

    expect(pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm/vehicle%engine%rpm/abc123", topic,
                                      schema, zid),
           "a five-segment advertisement parses through the zid overload");
    expect(zid == "abc123", "the fifth segment is the publisher's session id");

    // Set to empty, not left alone: a caller reusing the string across calls
    // would otherwise attribute one publisher's topic to a previous publisher.
    zid = "stale";
    expect(pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm/vehicle%engine%rpm", topic, schema,
                                      zid),
           "a four-segment advertisement still parses");
    expect(zid.empty(), "a missing zid is CLEARED, not left holding a previous call's value");

    // The two-argument form is the same parse, so an existing caller sees no
    // change whether or not the advertiser carries a zid.
    expect(pub_sub::parseAdvertiseKey("@redline/adv/EngineRpm/vehicle%engine%rpm/abc123", topic,
                                      schema),
           "the three-argument form accepts a five-segment key too");
    expect(topic == "vehicle/engine/rpm", "and reads the same topic out of it");
}

// ------------------------------------------------- the node and service spaces

// Both new spaces have to stay out of the way of every '**' subscriber in the
// tree. That is what the leading '@' does -- zenoh treats a segment beginning
// with '@' as verbatim, so no wildcard matches it. Without that property these
// would show up as topics in topic discovery, in `inspect list`, and in a bag
// recording subscribed to '**'.
void testNewSpacesAreInTheVerbatimSpace()
{
    expect(pub_sub::kNodePrefix.front() == '@',
           "the node space starts with '@', so '**' cannot match it");
    expect(pub_sub::kServicePrefix.front() == '@',
           "the service space starts with '@', so '**' cannot match it");

    expect(pub_sub::nodeKey("abc123", "carplay").front() == '@',
           "a built node key is in the verbatim space");
    expect(pub_sub::serviceKey("vehicle/can/set_bitrate", "Req", "Resp", "abc123").front() == '@',
           "a built service key is in the verbatim space");
}

void testNodeKeyRoundTrips()
{
    const std::string key = pub_sub::nodeKey("a1b2c3d4", "carplay");
    expect(key == "@redline/node/a1b2c3d4/carplay", "the node key has the documented shape");

    std::string zid;
    std::string name;
    expect(pub_sub::parseNodeKey(key, zid, name), "it parses back");
    expect(zid == "a1b2c3d4", "the zid survives");
    expect(name == "carplay", "the name survives");
}

void testMalformedNodeKeysAreRejected()
{
    std::string zid;
    std::string name;

    expect(!pub_sub::parseNodeKey("", zid, name), "an empty key is rejected");
    expect(!pub_sub::parseNodeKey("@redline/node/abc", zid, name),
           "a key missing the name segment is rejected");
    expect(!pub_sub::parseNodeKey("@redline/adv/abc/name", zid, name),
           "an advertisement is not a node key");
    expect(!pub_sub::parseNodeKey("@redline/node//name", zid, name),
           "an empty zid segment is rejected");
    expect(!pub_sub::parseNodeKey("@redline/node/abc/", zid, name),
           "an empty name segment is rejected");

    // Append-only, from the first version: a future field must not blank out
    // every node for every build that predates it.
    expect(pub_sub::parseNodeKey("@redline/node/abc/carplay/pid/1234", zid, name),
           "extra segments are ignored rather than fatal");
    expect(zid == "abc" && name == "carplay", "and the known segments are read correctly");
}

void testServiceKeyRoundTrips()
{
    const std::string key = pub_sub::serviceKey("vehicle/can/set_bitrate",
                                                "CanBridgeSetBitrateRequest",
                                                "CanBridgeSetBitrateResponse", "a1b2c3d4");
    expect(key == "@redline/svc/a1b2c3d4/CanBridgeSetBitrateRequest/"
                  "CanBridgeSetBitrateResponse/vehicle%can%set_bitrate",
           "the service key has the documented shape, with the key mangled");

    std::string keyexpr;
    std::string request;
    std::string response;
    std::string zid;
    expect(pub_sub::parseServiceKey(key, keyexpr, request, response, zid), "it parses back");
    expect(keyexpr == "vehicle/can/set_bitrate", "the service key expression demangles");
    expect(request == "CanBridgeSetBitrateRequest", "the request schema survives");
    expect(response == "CanBridgeSetBitrateResponse", "the response schema survives");
    expect(zid == "a1b2c3d4", "the owner zid survives");
}

void testMalformedServiceKeysAreRejected()
{
    std::string keyexpr;
    std::string request;
    std::string response;
    std::string zid;

    expect(!pub_sub::parseServiceKey("", keyexpr, request, response, zid),
           "an empty key is rejected");
    expect(!pub_sub::parseServiceKey("@redline/svc/zid/Req/Resp", keyexpr, request, response, zid),
           "a key missing the service key segment is rejected");
    expect(!pub_sub::parseServiceKey("@redline/adv/Schema/topic", keyexpr, request, response, zid),
           "a topic advertisement is not a service key");
    expect(!pub_sub::parseServiceKey("@redline/svc/zid//Resp/key", keyexpr, request, response, zid),
           "an empty request schema is rejected");

    // A service key segment that does not demangle into something callable
    // means the advertiser broke the contract; offering it would produce a
    // request that can never be routed.
    expect(!pub_sub::parseServiceKey("@redline/svc/zid/Req/Resp/%leading", keyexpr, request,
                                     response, zid),
           "a key that demangles into an invalid topic key is rejected");

    expect(pub_sub::parseServiceKey("@redline/svc/zid/Req/Resp/a%b/extra", keyexpr, request,
                                    response, zid),
           "extra segments are ignored rather than fatal");
    expect(keyexpr == "a/b", "and the known segments are read correctly");
}

// The three spaces must not be confusable with each other. Each parser has to
// reject the other two, or a node would be listed as a topic and a service as a
// node -- all of which look like plausible entries rather than errors.
void testTheThreeSpacesDoNotOverlap()
{
    const std::string advertisement = pub_sub::advertiseKey("vehicle/engine/rpm", "EngineRpm", "z1");
    const std::string node = pub_sub::nodeKey("z1", "carplay");
    const std::string service = pub_sub::serviceKey("vehicle/svc", "Req", "Resp", "z1");

    std::string a;
    std::string b;
    std::string c;
    std::string d;

    expect(!pub_sub::parseNodeKey(advertisement, a, b), "an advertisement is not a node");
    expect(!pub_sub::parseServiceKey(advertisement, a, b, c, d),
           "an advertisement is not a service");

    expect(!pub_sub::parseAdvertiseKey(node, a, b), "a node is not an advertisement");
    expect(!pub_sub::parseServiceKey(node, a, b, c, d), "a node is not a service");

    expect(!pub_sub::parseAdvertiseKey(service, a, b), "a service is not an advertisement");
    expect(!pub_sub::parseNodeKey(service, a, b), "a service is not a node");
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
    testExtraSegmentsAreTolerated();
    testZidOverload();
    testNewSpacesAreInTheVerbatimSpace();
    testNodeKeyRoundTrips();
    testMalformedNodeKeysAreRejected();
    testServiceKeyRoundTrips();
    testMalformedServiceKeysAreRejected();
    testTheThreeSpacesDoNotOverlap();
    testMalformedAdvertisementsAreRejected();

    testEveryShippedKeyRoundTrips();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
