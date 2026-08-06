#ifndef CLI_PROGRAM_H_
#define CLI_PROGRAM_H_

#include <cxxopts.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cli
{

// Exit codes, spelled out because scripts read them and because getting this
// wrong is silent.
//
// nodes/inspect used to `return 0` when a required option was missing -- it
// printed usage and reported success, so `inspect dump && do_something` ran
// `do_something` after a command that did nothing. kUsage exists to make that
// distinguishable from kFailure: "you asked wrongly" and "I tried and could
// not" are different answers and a caller may want to retry only one of them.
inline constexpr int kOk = 0;
inline constexpr int kFailure = 1;
inline constexpr int kUsage = 2;

class Context;

// One subcommand.
//
// `add_options` and `run` are separate on purpose: `<prog> <verb> --help` has to
// render a verb's options without running it, and a single entry point would
// have meant every verb starting with an "am I only being asked for help?"
// branch -- which is exactly the copy-pasted block this type exists to delete.
struct Verb
{
    std::string_view name;

    // One line, shown in the verb list. No trailing period; the list aligns on
    // it.
    std::string_view summary;

    // Declares this verb's own options on `options`. Global options are already
    // present, so a verb must not redeclare them -- cxxopts throws at startup if
    // it does, which is the right time to find out.
    //
    // A verb that takes a positional calls options.parse_positional() here.
    void (*add_options)(cxxopts::Options& options);

    int (*run)(Context& context);
};

// What a verb is handed: the parsed command line, plus the global answers.
//
// Globals and verb options come out of ONE parse, so `inspect --debug list` and
// `inspect list --debug` are the same command. The alternative -- parsing the
// tokens before the verb separately from the ones after it -- is what the old
// inspect did, and it is why `--debug` never worked in either position.
class Context
{
  public:
    Context(const cxxopts::ParseResult& parsed, const cxxopts::Options& options,
            std::string_view verb);

    // The whole parse. Prefer the accessors below where they fit; this is here
    // for options whose shape they do not cover (repeated values, counts).
    const cxxopts::ParseResult& args() const { return parsed_; }

    bool has(const std::string& name) const { return parsed_.count(name) != 0; }

    // Globals, resolved once so no verb has to know how they are spelled.
    bool json() const { return json_; }
    bool debug() const { return debug_; }

    // "inspect list", for a message that names the command the user typed.
    std::string_view verb() const { return verb_; }

    // A required option, or std::nullopt having already reported *what* is
    // missing and printed the verb's usage. The caller returns kUsage:
    //
    //     const auto key = context.requireString("key");
    //     if (!key) { return kUsage; }
    //
    // Returning an optional rather than throwing keeps the failure on the
    // normal path, where a verb can still clean up after itself.
    std::optional<std::string> requireString(const std::string& name) const;

    // An optional option with a fallback, without the count()/as<>() dance.
    std::string stringOr(const std::string& name, std::string fallback) const;
    std::uint64_t uintOr(const std::string& name, std::uint64_t fallback) const;
    double doubleOr(const std::string& name, double fallback) const;
    bool flag(const std::string& name) const;

    // This verb's usage text, for a verb reporting a problem the option parser
    // cannot see (two mutually exclusive flags, say).
    void printUsage() const;

  private:
    const cxxopts::ParseResult& parsed_;
    const cxxopts::Options& options_;
    std::string_view verb_;
    bool json_ = false;
    bool debug_ = false;
};

// The program: a name, a description, and a table of verbs.
//
// Adding a verb is one row in that table. The old inspect needed four edits --
// a source file, a header, an include plus an if-branch in main(), and a
// hand-maintained help string -- and the help string was stale in two places by
// the time anyone noticed, listing two verbs out of five.
class Program
{
  public:
    Program(std::string_view name, std::string_view description, std::span<const Verb> verbs);

    // Parses, dispatches, and returns the process exit code. Everything that
    // can go wrong before a verb runs -- no verb, unknown verb, a bad option,
    // a leftover argument -- is reported here and returns kUsage.
    int run(int argc, char** argv) const;

  private:
    const Verb* findVerb(std::string_view name) const;

    // The verb list, for `<prog>` with no arguments and for `<prog> --help`.
    void printVerbList() const;

    // "Unknown verb 'lst'. Did you mean 'list'?" -- empty when nothing is close.
    std::string_view nearestVerb(std::string_view name) const;

    std::string_view name_;
    std::string_view description_;
    std::span<const Verb> verbs_;
};

}  // namespace cli

#endif  // CLI_PROGRAM_H_
