#include "cli/program.h"

#include "cli/output.h"
#include "cli/session_options.h"

#include "core/core.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <vector>

namespace cli
{

namespace
{

// The global options, in one place.
//
// They are added to the *same* cxxopts::Options as the verb's own, so a global
// parses identically whether it appears before or after the verb. See the
// comment on Context.
void addGlobalOptions(cxxopts::Options& options)
{
    options.add_options("global")
        ("v,debug", "Enable debug logging.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("json", "Emit results as JSON on stdout, for scripting.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("connect", "Zenoh endpoint to connect to, e.g. tcp/192.168.1.10:7447. "
                    "Repeatable. Default is peer-mode discovery.",
            cxxopts::value<std::vector<std::string>>())
        ("mode", "Zenoh session mode: peer or client.",
            cxxopts::value<std::string>())
        ("h,help", "Print usage");
}

// Globals that consume the *next* argv token as their value.
//
// This list is what makes the verb scan exact rather than a guess: without it,
// `inspect --mode client list` would take "client" for the verb. A global added
// above with a value type has to be added here too -- which is a real trap, so
// verifyGlobalValueTable() below fails the build's own test if the two drift.
constexpr std::string_view kValueTakingGlobals[] = {"--connect", "--mode"};

bool takesSeparateValue(std::string_view token)
{
    return std::ranges::find(kValueTakingGlobals, token) != std::ranges::end(kValueTakingGlobals);
}

// The index of the verb token in argv, or nullopt when there is none.
//
// Left to right, skipping options and the values they consume. The first bare
// token left is the verb. `--` is skipped like any other dashed token, so
// `prog -- list` names the verb `list`.
std::optional<int> findVerbIndex(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view token(argv[i]);
        if (token.empty())
        {
            continue;
        }

        if (token.starts_with('-'))
        {
            // '--mode=client' carries its value; '--mode client' eats the next
            // token.
            if (token.find('=') == std::string_view::npos && takesSeparateValue(token))
            {
                ++i;
            }
            continue;
        }

        return i;
    }

    return std::nullopt;
}

// Levenshtein distance, capped -- only ever used to decide whether a typo is
// close enough to suggest.
std::size_t editDistance(std::string_view lhs, std::string_view rhs)
{
    std::vector<std::size_t> previous(rhs.size() + 1);
    std::vector<std::size_t> current(rhs.size() + 1);

    for (std::size_t j = 0; j <= rhs.size(); ++j)
    {
        previous[j] = j;
    }

    for (std::size_t i = 1; i <= lhs.size(); ++i)
    {
        current[0] = i;
        for (std::size_t j = 1; j <= rhs.size(); ++j)
        {
            const std::size_t substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0u : 1u);
            current[j] = std::min({current[j - 1] + 1u, previous[j] + 1u, substitution});
        }
        previous.swap(current);
    }

    return previous[rhs.size()];
}

}  // namespace

Context::Context(const cxxopts::ParseResult& parsed, const cxxopts::Options& options,
                 std::string_view verb) :
    parsed_(parsed),
    options_(options),
    verb_(verb),
    json_(parsed["json"].as<bool>()),
    debug_(parsed["debug"].as<bool>())
{
}

std::optional<std::string> Context::requireString(const std::string& name) const
{
    if (parsed_.count(name) == 0)
    {
        SPDLOG_ERROR("'{}' requires --{}.", verb_, name);
        printUsage();
        return std::nullopt;
    }

    return parsed_[name].as<std::string>();
}

std::string Context::stringOr(const std::string& name, std::string fallback) const
{
    return parsed_.count(name) != 0 ? parsed_[name].as<std::string>() : std::move(fallback);
}

std::uint64_t Context::uintOr(const std::string& name, std::uint64_t fallback) const
{
    return parsed_.count(name) != 0 ? parsed_[name].as<std::uint64_t>() : fallback;
}

double Context::doubleOr(const std::string& name, double fallback) const
{
    return parsed_.count(name) != 0 ? parsed_[name].as<double>() : fallback;
}

bool Context::flag(const std::string& name) const
{
    return parsed_.count(name) != 0 && parsed_[name].as<bool>();
}

void Context::printUsage() const
{
    // Usage is a result, not a diagnostic: someone who typed --help asked for
    // it, and piping it into a pager should not carry log decoration.
    out("{}", options_.help());
}

Program::Program(std::string_view name, std::string_view description,
                 std::span<const Verb> verbs) :
    name_(name),
    description_(description),
    verbs_(verbs)
{
}

const Verb* Program::findVerb(std::string_view name) const
{
    const auto it = std::ranges::find_if(verbs_, [name](const Verb& verb)
                                         { return verb.name == name; });
    return it != verbs_.end() ? &*it : nullptr;
}

std::string_view Program::nearestVerb(std::string_view name) const
{
    std::string_view best;
    std::size_t best_distance = 3;  // Anything further away is not a typo.

    for (const Verb& verb : verbs_)
    {
        const std::size_t distance = editDistance(name, verb.name);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = verb.name;
        }
    }

    return best;
}

void Program::printVerbList() const
{
    std::size_t width = 0;
    for (const Verb& verb : verbs_)
    {
        width = std::max(width, verb.name.size());
    }

    out("{} -- {}", name_, description_);
    out("");
    out("Usage: {} <verb> [options]", name_);
    out("");
    out("Verbs:");
    for (const Verb& verb : verbs_)
    {
        out("  {:<{}}  {}", verb.name, width, verb.summary);
    }
    out("");
    out("Run '{} <verb> --help' for a verb's options.", name_);
}

int Program::run(int argc, char** argv) const
{
    const std::optional<int> verb_index = findVerbIndex(argc, argv);

    if (!verb_index)
    {
        // No verb. `--help` asked for the list; anything else is a user who
        // does not yet know what this program does -- so print the same list,
        // but say it was a usage error so a script can tell.
        //
        // The old inspect crashed here: it read the `verb` positional
        // unconditionally and cxxopts throws option_has_no_value.
        printVerbList();

        const bool asked_for_help =
            std::any_of(argv + 1, argv + argc,
                        [](const char* token)
                        {
                            const std::string_view view(token);
                            return view == "-h" || view == "--help";
                        });
        return asked_for_help ? kOk : kUsage;
    }

    std::string_view verb_name(argv[*verb_index]);

    // `<prog> help <verb>` and `<prog> help` -- the form people try before they
    // try `--help`.
    if (verb_name == "help")
    {
        const std::optional<int> topic_index = findVerbIndex(argc - *verb_index, argv + *verb_index);
        if (!topic_index)
        {
            printVerbList();
            return kOk;
        }
        verb_name = std::string_view(argv[*verb_index + *topic_index]);

        const Verb* topic = findVerb(verb_name);
        if (topic == nullptr)
        {
            SPDLOG_ERROR("Unknown verb '{}'.", verb_name);
            printVerbList();
            return kUsage;
        }

        cxxopts::Options options(std::string(name_) + " " + std::string(topic->name),
                                 std::string(topic->summary));
        addGlobalOptions(options);
        topic->add_options(options);
        out("{}", options.help());
        return kOk;
    }

    const Verb* verb = findVerb(verb_name);
    if (verb == nullptr)
    {
        SPDLOG_ERROR("Unknown verb '{}'.", verb_name);
        const std::string_view suggestion = nearestVerb(verb_name);
        if (!suggestion.empty())
        {
            SPDLOG_ERROR("Did you mean '{}'?", suggestion);
        }
        printVerbList();
        return kUsage;
    }

    // One parse for globals and verb options together. The verb token itself is
    // removed rather than declared as a positional, so a verb is free to declare
    // positionals of its own without colliding with it.
    const std::string program_name = std::string(name_) + " " + std::string(verb->name);
    std::vector<char*> forwarded;
    forwarded.reserve(static_cast<std::size_t>(argc));
    forwarded.push_back(argv[0]);
    for (int i = 1; i < argc; ++i)
    {
        if (i != *verb_index)
        {
            forwarded.push_back(argv[i]);
        }
    }

    cxxopts::Options options(program_name, std::string(verb->summary));
    addGlobalOptions(options);
    verb->add_options(options);

    cxxopts::ParseResult parsed;
    try
    {
        int forwarded_argc = static_cast<int>(forwarded.size());
        char** forwarded_argv = forwarded.data();
        parsed = options.parse(forwarded_argc, forwarded_argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        out("{}", options.help());
        return kUsage;
    }

    // Logging is configured before the verb runs but after --debug is known, so
    // a verb's own SPDLOG_DEBUG lines are subject to it.
    core::setupLogging(parsed["debug"].as<bool>());

    if (parsed.count("help") != 0)
    {
        out("{}", options.help());
        return kOk;
    }

    // Refuse leftovers rather than ignoring them. The trap, which cost an
    // afternoon in the dashboard and again in scope: a cxxopts option with an
    // implicit value does NOT consume a space-separated argument, so
    // `--mcp /tmp/a.sock` silently uses the default path and drops the one you
    // asked for. Here it also catches a stray positional a verb did not declare,
    // which the old inspect swallowed into unmatched() and ignored.
    if (!parsed.unmatched().empty())
    {
        for (const std::string& leftover : parsed.unmatched())
        {
            SPDLOG_ERROR("Unrecognised argument '{}'.", leftover);
        }
        out("{}", options.help());
        return kUsage;
    }

    if (parsed.count("connect") != 0 || parsed.count("mode") != 0)
    {
        // Applied before any verb opens a session. SessionManager caches the
        // session process-wide, so this has to happen before the first
        // getOrCreate() or it is silently ignored -- which is why it lives here
        // and not in each verb.
        applySessionOverrides(parsed);
    }

    Context context(parsed, options, verb->name);

    try
    {
        return verb->run(context);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{} {}: {}", name_, verb->name, e.what());
        return kFailure;
    }
}

}  // namespace cli
