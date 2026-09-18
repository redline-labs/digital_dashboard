// The backlight node's config: defaults, overrides, and the typos it refuses.

#include "node_config.h"

#include <spdlog/spdlog.h>

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

using backlight_node::NodeConfig;
using backlight_node::parse_node_config;

bool parses(const std::string& yaml, NodeConfig& out)
{
    return parse_node_config(yaml, out);
}

bool refuses(const std::string& yaml)
{
    NodeConfig ignored;
    return !parse_node_config(yaml, ignored);
}

void testDefaults()
{
    NodeConfig config;
    check(parses("", config), "an empty file is every default");
    check(config.role == "primary", "the primary display by default");
    check(config.recordDir == "/run/redline/displays", "the rootfs's record directory by default");
    check(config.resolvedTopicPrefix() == "nodes/backlight/primary", "the prefix follows the role");
    check(config.pollMs == 500 && config.minPercent == 1.0, "poll and floor defaults");
    check(config.lightIntegrationTime == 0.1, "the opt3001's short integration time by default");
}

void testOverrides()
{
    NodeConfig config;
    check(parses("role: secondary\nrecord_dir: /tmp/fake\npoll_ms: 250\nmin_percent: 5\n", config),
          "a full config parses");
    check(config.role == "secondary" && config.recordDir == "/tmp/fake", "strings are read");
    check(config.resolvedTopicPrefix() == "nodes/backlight/secondary", "the default prefix follows the role");
    check(config.pollMs == 250 && config.minPercent == 5.0, "numbers are read");

    NodeConfig integration;
    check(parses("light_integration_time: 0.8\n", integration) && integration.lightIntegrationTime == 0.8,
          "an integration time is read");
    NodeConfig leave;
    check(parses("light_integration_time: 0\n", leave) && leave.lightIntegrationTime == 0.0,
          "zero, meaning leave the driver's, is accepted");

    NodeConfig prefixed;
    check(parses("topic_prefix: cluster/backlight\n", prefixed) &&
              prefixed.resolvedTopicPrefix() == "cluster/backlight",
          "an explicit prefix wins");
}

void testRefusals()
{
    check(refuses("rol: primary\n"), "an unknown key");
    check(refuses("role: tertiary\n"), "a role the rootfs never writes a record for");
    check(refuses("poll_ms: 0\n"), "a zero poll period");
    check(refuses("poll_ms: fast\n"), "a poll period that is not a number");
    check(refuses("min_percent: 150\n"), "a floor above 100%");
    check(refuses("min_percent: -1\n"), "a negative floor");
    check(refuses("record_dir: \"\"\n"), "an empty record directory");
    check(refuses("light_integration_time: -0.1\n"), "a negative integration time");
    check(refuses("light_integration_time: .nan\n"), "a NaN integration time");
    check(refuses("light_integration_time: slow\n"), "an integration time that is not a number");
    check(refuses("topic_prefix: \"nodes/back light\"\n"), "a prefix that is not a usable zenoh key");
    check(refuses("[1, 2]\n"), "a top level that is not a mapping");
}

}  // namespace

int main()
{
    // The refusals log errors by design; they are not what this run is about.
    spdlog::set_level(spdlog::level::off);

    testDefaults();
    testOverrides();
    testRefusals();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
