// SPDX-License-Identifier: GPL-3.0-or-later
//
// NodeCommandLine: the flags every node shares, spelled every way a deployed
// .args file might spell them, and the exit codes a script relies on.

#include "cli/node_options.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Parses `args` (program name first) with a fresh command line.
struct Parsed
{
    std::optional<int> exit;
    std::string config;
    bool debug = false;
};

Parsed parse(std::initializer_list<const char*> args, bool with_config = true,
             const char* default_config = "")
{
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : storage)
    {
        argv.push_back(arg.data());
    }
    cli::NodeCommandLine cli("node", "test node");
    if (with_config)
    {
        cli.withConfig(default_config);
    }
    cli.add()("extra", "a node's own option");
    Parsed parsed;
    parsed.exit = cli.parse(static_cast<int>(argv.size()), argv.data());
    parsed.config = cli.config();
    parsed.debug = cli.debug();
    spdlog::set_level(spdlog::level::off);
    return parsed;
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    expect(!parse({"node"}).exit, "no arguments carries on");
    expect(!parse({"node"}).debug, "and is not debug");
    for (const char* flag : {"-d", "--debug", "-v", "--verbose"})
    {
        const Parsed p = parse({"node", flag});
        expect(!p.exit && p.debug, std::string(flag) + " turns debug on");
    }

    expect(parse({"node", "-c", "a.yaml"}).config == "a.yaml", "-c names the config");
    expect(parse({"node", "--config", "b.yaml"}).config == "b.yaml", "--config names the config");
    expect(parse({"node"}, true, "default.yaml").config == "default.yaml", "the default applies");
    expect(parse({"node"}).config.empty(), "no default, no config");

    expect(parse({"node", "--help"}).exit == std::optional<int>(0), "--help exits 0");
    expect(parse({"node", "--no-such-flag"}).exit == std::optional<int>(2), "an unknown flag exits 2");
    expect(parse({"node", "--config"}).exit == std::optional<int>(2), "a missing value exits 2");
    expect(parse({"node", "-c", "x"}, false).exit == std::optional<int>(2),
           "--config is refused by a node that takes none");
    expect(!parse({"node", "--extra"}).exit, "the node's own options still parse");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
