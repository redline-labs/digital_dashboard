// SPDX-License-Identifier: GPL-3.0-or-later
//
// The --mcp option every GUI app declares through agent_control. The case with
// history is `--mcp /tmp/a.sock`: an implicit-value option does not consume a
// space-separated argument, so without the leftover check the app listened on
// the default path and silently dropped the one it was given.

#include "agent_control/app_bootstrap.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
#include <vector>

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

cxxopts::ParseResult parse(std::vector<std::string> args)
{
    cxxopts::Options options("test", "");
    agent_control::addMcpOption(options, "test");
    args.insert(args.begin(), "test");
    std::vector<char*> argv;
    for (std::string& arg : args)
    {
        argv.push_back(arg.data());
    }
    options.allow_unrecognised_options();
    return options.parse(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    {
        const auto result = parse({});
        check(!agent_control::mcpSocketPath(result, "test").has_value(), "no --mcp, no socket");
        check(agent_control::rejectUnmatched(result), "and nothing left over");
    }
    {
        const auto result = parse({"--mcp"});
        const auto path = agent_control::mcpSocketPath(result, "test");
        check(path.has_value() && path->starts_with("/tmp/redline_test_") && path->ends_with(".sock"),
              "bare --mcp listens on the per-app default");
    }
    {
        const auto result = parse({"--mcp=/tmp/a.sock"});
        check(agent_control::mcpSocketPath(result, "test") == std::optional<std::string>("/tmp/a.sock"),
              "--mcp=<path> listens there");
    }
    {
        const auto result = parse({"--mcp", "/tmp/a.sock"});
        check(!agent_control::rejectUnmatched(result),
              "--mcp <path> with a space is refused rather than listening on the default");
    }

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
