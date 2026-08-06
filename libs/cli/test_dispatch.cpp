// SPDX-License-Identifier: GPL-3.0-or-later
//
// Verb dispatch and the globals/verb-options split.
//
// This is the regression test for three defects the old nodes/inspect shipped,
// each of which is the kind that a compiler cannot see and a casual run does not
// show:
//
//   1. `inspect` with no arguments ABORTED. main.cpp read the `verb` positional
//      unconditionally, outside any try block, and cxxopts throws
//      option_has_no_value:
//        libc++abi: terminating due to uncaught exception of type
//        cxxopts::exceptions::option_has_no_value: Option 'verb' has no value
//
//   2. `--debug` was documented in the top-level help and rejected by every
//      verb. It was declared only on the top-level parser; the verb parsers did
//      not allow_unrecognised_options(), so both `inspect --debug list` and
//      `inspect list --debug` failed with "Option 'debug' does not exist". The
//      flag worked only when no verb was given, i.e. never usefully.
//
//   3. A missing required option returned EXIT SUCCESS. Every verb did
//      `if (result.count("help") || !result.count("key")) { print usage;
//      return 0; }`, so `inspect dump && something` ran `something` after a
//      command that did nothing at all.
//
// Mutation-check: revert Program::run to a bare if-chain over argv[1] and this
// file must fail. Cases 1 and 3 in particular pass trivially against code that
// looks reasonable, which is why they are asserted by exit code rather than by
// eyeballing output.

#include "cli/program.h"

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

// ------------------------------------------------------- the fixture program

// What the last-run verb saw. Verb entry points are plain function pointers, so
// this is how the test observes them.
struct Observed
{
    bool ran = false;
    bool debug = false;
    bool json = false;
    std::string key;
    std::string positional;
};

Observed g_observed;

void addAlphaOptions(cxxopts::Options& options)
{
    options.add_options()
        ("k,key", "A key.", cxxopts::value<std::string>());
}

int runAlpha(cli::Context& context)
{
    g_observed.ran = true;
    g_observed.debug = context.debug();
    g_observed.json = context.json();

    const auto key = context.requireString("key");
    if (!key)
    {
        return cli::kUsage;
    }
    g_observed.key = *key;
    return cli::kOk;
}

// A verb with a positional, which is the shape `echo <key>` and `bag info <dir>`
// need. The verb token must not collide with it -- Program removes the verb from
// argv rather than declaring it as a positional of its own, and that is what
// this exercises.
void addBetaOptions(cxxopts::Options& options)
{
    options.add_options()
        ("target", "A positional.", cxxopts::value<std::string>());
    options.parse_positional({"target"});
}

int runBeta(cli::Context& context)
{
    g_observed.ran = true;
    g_observed.positional = context.stringOr("target", "");
    return cli::kOk;
}

int runFailing(cli::Context& /*context*/)
{
    g_observed.ran = true;
    return cli::kFailure;
}

void addNoOptions(cxxopts::Options& /*options*/) {}

int runThrowing(cli::Context& /*context*/)
{
    g_observed.ran = true;
    throw std::runtime_error("something came apart");
}

constexpr cli::Verb kVerbs[] = {
    {"alpha", "First verb", addAlphaOptions, runAlpha},
    {"beta", "Second verb, takes a positional", addBetaOptions, runBeta},
    {"broken", "Always fails", addNoOptions, runFailing},
    {"throwing", "Always throws", addNoOptions, runThrowing},
};

// Runs the fixture program over a synthetic command line. argv strings are held
// in a vector<string> so the char* handed to Program stay valid for the call.
int invoke(std::vector<std::string> arguments)
{
    g_observed = Observed{};

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1u);
    storage.emplace_back("testprog");
    for (auto& argument : arguments)
    {
        storage.push_back(std::move(argument));
    }

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& item : storage)
    {
        argv.push_back(item.data());
    }

    const cli::Program program("testprog", "A fixture.", kVerbs);
    return program.run(static_cast<int>(argv.size()), argv.data());
}

// ---------------------------------------------------------------- the cases

// Defect 1. The assertion that matters is that this RETURNS at all -- an abort
// takes the whole test binary with it, which ctest reports as a failure, so the
// bug is caught either way.
void testNoVerb()
{
    expect(invoke({}) == cli::kUsage, "no verb is a usage error, not a crash");
    expect(!g_observed.ran, "no verb runs nothing");

    expect(invoke({"--help"}) == cli::kOk, "--help alone succeeds");
    expect(invoke({"-h"}) == cli::kOk, "-h alone succeeds");

    // Globals with no verb are still not a command.
    expect(invoke({"--debug"}) == cli::kUsage, "globals without a verb are a usage error");
}

void testUnknownVerb()
{
    expect(invoke({"nosuchverb"}) == cli::kUsage, "an unknown verb is a usage error");
    expect(!g_observed.ran, "an unknown verb runs nothing");

    // Near-miss suggestion. Only the exit code is asserted; the suggestion goes
    // to the log, and asserting on log text would pin the wording rather than
    // the behaviour.
    expect(invoke({"alpah"}) == cli::kUsage, "a typo is a usage error");
}

// Defect 2, both orders. This is the whole reason globals and verb options go
// into one cxxopts::Options and the verb token is deleted from argv.
void testGlobalsInEitherPosition()
{
    expect(invoke({"--debug", "alpha", "--key", "a/b"}) == cli::kOk,
           "a global BEFORE the verb parses");
    expect(g_observed.debug, "--debug before the verb reaches the verb");

    expect(invoke({"alpha", "--debug", "--key", "a/b"}) == cli::kOk,
           "a global AFTER the verb parses");
    expect(g_observed.debug, "--debug after the verb reaches the verb");

    expect(invoke({"alpha", "--key", "a/b"}) == cli::kOk, "no --debug is fine");
    expect(!g_observed.debug, "--debug defaults to false");

    expect(invoke({"--json", "alpha", "--key", "a/b"}) == cli::kOk, "--json before the verb");
    expect(g_observed.json, "--json reaches the verb");

    expect(invoke({"-v", "alpha", "--key", "a/b"}) == cli::kOk, "the short form -v works");
    expect(g_observed.debug, "-v is --debug");
}

// A value-taking global must not have its value mistaken for the verb. Without
// the kValueTakingGlobals table in program.cpp, "client" here would be taken as
// the verb name and the command would fail as unknown.
void testValueTakingGlobalBeforeVerb()
{
    expect(invoke({"--mode", "client", "alpha", "--key", "a/b"}) == cli::kOk,
           "--mode <value> before the verb does not swallow the verb");
    expect(g_observed.ran, "the verb still ran");

    expect(invoke({"--mode=client", "alpha", "--key", "a/b"}) == cli::kOk,
           "--mode=<value> before the verb");
    expect(g_observed.ran, "the verb still ran with the '=' form");

    expect(invoke({"--connect", "tcp/127.0.0.1:7447", "alpha", "--key", "a/b"}) == cli::kOk,
           "--connect <value> before the verb");
    expect(g_observed.ran, "the verb still ran after --connect");
}

// Defect 3.
void testMissingRequiredOptionIsUsage()
{
    expect(invoke({"alpha"}) == cli::kUsage,
           "a missing required option is a usage error, NOT success");
    expect(g_observed.ran, "the verb ran and decided for itself");
    expect(g_observed.key.empty(), "the verb did not get a key");
}

void testVerbHelp()
{
    expect(invoke({"alpha", "--help"}) == cli::kOk, "<verb> --help succeeds");
    expect(!g_observed.ran, "<verb> --help does not run the verb");

    expect(invoke({"help", "alpha"}) == cli::kOk, "help <verb> succeeds");
    expect(!g_observed.ran, "help <verb> does not run the verb");

    expect(invoke({"help"}) == cli::kOk, "bare help succeeds");
    expect(invoke({"help", "nosuchverb"}) == cli::kUsage, "help for an unknown verb is a usage error");
}

void testPositionals()
{
    expect(invoke({"beta", "some/target"}) == cli::kOk, "a verb's positional parses");
    expect(g_observed.positional == "some/target",
           "the positional is the argument, not the verb name");

    expect(invoke({"--debug", "beta", "some/target"}) == cli::kOk,
           "a global and a positional coexist");
    expect(g_observed.positional == "some/target", "the global did not shift the positional");
}

// A stray argument is refused rather than silently ignored. The old inspect
// dropped these into unmatched() and never looked, which is why the documented
// `inspect hz <key>` form appeared to work and did nothing.
void testUnmatchedArgumentsRefused()
{
    expect(invoke({"alpha", "--key", "a/b", "stray"}) == cli::kUsage,
           "a stray positional a verb did not declare is a usage error");
}

void testBadOptionIsUsage()
{
    expect(invoke({"alpha", "--nosuchoption"}) == cli::kUsage,
           "an option that does not exist is a usage error");
    expect(!g_observed.ran, "a bad option does not reach the verb");
}

void testExitCodesArePassedThrough()
{
    expect(invoke({"broken"}) == cli::kFailure, "a verb's kFailure is the process exit code");
    expect(g_observed.ran, "the failing verb ran");
}

// An exception escaping a verb is a failure, not a crash. Zenoh, capnp and the
// filesystem all throw, and a tool that aborts loses the diagnostic.
void testThrowingVerbIsCaught()
{
    expect(invoke({"throwing"}) == cli::kFailure, "an exception out of a verb becomes kFailure");
}

}  // namespace

int main()
{
    testNoVerb();
    testUnknownVerb();
    testGlobalsInEitherPosition();
    testValueTakingGlobalBeforeVerb();
    testMissingRequiredOptionIsUsage();
    testVerbHelp();
    testPositionals();
    testUnmatchedArgumentsRefused();
    testBadOptionIsUsage();
    testExitCodesArePassedThrough();
    testThrowingVerbIsCaught();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
