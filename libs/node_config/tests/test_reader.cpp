// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared node-config readers against the input a hand-edited YAML file
// actually contains: the wrong type, a negative number where only a positive
// one makes sense, a number too large for its field, a list where a scalar
// belongs, infinity, a key the bus would silently ignore.
//
// Every case checks two things: that the failure is recorded, and that the
// field kept its previous value -- a reader that half-applies a bad value
// leaves a node running on something nobody configured.

#include "node_config/reader.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
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

void testUnsigned()
{
    const YAML::Node yaml = YAML::Load("ok: 42\nneg: -1\nbig: 300\nword: fast\nlist: [1, 2]\n");

    node_config::Context context;
    std::uint8_t value = 7;
    node_config::readUint(yaml, "ok", value, context, "x");
    check(context.ok && value == 42, "a plain integer reads");

    for (const char* key : {"neg", "big", "word", "list"})
    {
        node_config::Context bad;
        std::uint8_t kept = 7;
        node_config::readUint(yaml, key, kept, bad, "x");
        check(!bad.ok && kept == 7, std::string("'") + key + "' into a uint8_t is refused and leaves the field alone");
    }

    node_config::Context absent;
    std::uint16_t untouched = 9;
    node_config::readUint(yaml, "missing", untouched, absent, "x");
    check(absent.ok && untouched == 9, "an absent key is not a failure and changes nothing");
}

void testNumber()
{
    const YAML::Node yaml = YAML::Load("ok: -2.5\ninf: .inf\nnan: .nan\nword: soon\nint: -3\n");

    node_config::Context context;
    double value = 1.0;
    node_config::readNumber(yaml, "ok", value, context, "x");
    check(context.ok && value == -2.5, "a negative double reads");

    std::int32_t integer = 0;
    node_config::readNumber(yaml, "int", integer, context, "x");
    check(context.ok && integer == -3, "a signed integer reads");

    for (const char* key : {"inf", "nan", "word"})
    {
        node_config::Context bad;
        double kept = 1.0;
        node_config::readNumber(yaml, key, kept, bad, "x");
        check(!bad.ok && kept == 1.0, std::string("'") + key + "' is refused and leaves the field alone");
    }
}

void testStringAndBool()
{
    const YAML::Node yaml = YAML::Load("name: radio\nlist: [a]\nmap: {a: 1}\nflag: true\nmaybe: sometimes\n");

    node_config::Context context;
    std::string name = "default";
    node_config::readString(yaml, "name", name, context, "");
    check(context.ok && name == "radio", "a string reads");

    for (const char* key : {"list", "map"})
    {
        node_config::Context bad;
        std::string kept = "default";
        node_config::readString(yaml, key, kept, bad, "");
        check(!bad.ok && kept == "default", std::string("a ") + key + " where a string belongs is refused");
    }

    bool flag = false;
    node_config::readBool(yaml, "flag", flag, context, "");
    check(context.ok && flag, "a bool reads");

    node_config::Context bad;
    bool kept = false;
    node_config::readBool(yaml, "maybe", kept, bad, "");
    check(!bad.ok && !kept, "a word that is not a bool is refused");
}

void testTopicKeys()
{
    node_config::Context context;
    node_config::checkTopicKey("nodes/radio/status", "publish.status_key", context);
    check(context.ok, "a good key passes");

    for (const char* key : {"nodes/@radio", "nodes/radio*", "", "nodes/radio/"})
    {
        node_config::Context bad;
        node_config::checkTopicKey(key, "publish.status_key", bad);
        check(!bad.ok, std::string("'") + key + "' is refused");
    }
}

void testFieldPaths()
{
    check(node_config::fieldPath("", "port") == "port", "a top-level field is just its name");
    check(node_config::fieldPath("radio", "port") == "radio.port", "a nested one is dotted");
    check(node_config::fieldPath("inputs.", "imu") == "inputs.imu", "a trailing dot is not doubled");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    testUnsigned();
    testNumber();
    testStringAndBool();
    testTopicKeys();
    testFieldPaths();
    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
