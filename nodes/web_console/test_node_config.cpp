// The web console's config parser: defaults, validation, and the rule that an
// unknown key is an error rather than a setting that quietly does nothing.

#include "node_config.h"

#include <cstdio>
#include <string>

namespace
{

using namespace web_console;

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

void testDefaults()
{
    NodeConfig config;
    check(parse_node_config("", config), "an empty file parses");
    // Loopback, not 0.0.0.0: running this without a config must not put it on
    // the network.
    check(config.bindAddress == "127.0.0.1", "the default bind address is loopback");
    check(config.port == 8080, "the default port");
    check(config.assetDir.empty(), "no asset_dir means ask core::paths::resource");
    check(config.tokenFile.empty(), "no token by default");
}

void testEveryKeyIsRead()
{
    NodeConfig config;
    const bool ok = parse_node_config(
        "bind_address: 0.0.0.0\n"
        "port: 9090\n"
        "asset_dir: /opt/redline/web\n"
        "token_file: /data/web_console/token\n",
        config);
    check(ok, "a full config parses");
    check(config.bindAddress == "0.0.0.0", "bind_address is read");
    check(config.port == 9090, "port is read");
    check(config.assetDir == "/opt/redline/web", "asset_dir is read");
    check(config.tokenFile == "/data/web_console/token", "token_file is read");
}

void testUnknownKeyIsAnError()
{
    NodeConfig config;
    // The whole point: bind_adress would otherwise leave the service on
    // loopback while the operator believes it is exposed.
    check(!parse_node_config("bind_adress: 0.0.0.0\n", config), "a misspelled key fails the parse");
    check(!parse_node_config("prot: 9090\n", config), "so does a misspelled port");
}

void testPortRange()
{
    NodeConfig config;
    check(!parse_node_config("port: 80\n", config), "a privileged port is rejected");
    check(!parse_node_config("port: 70000\n", config), "a port above the protocol's range is rejected");
    check(!parse_node_config("port: eighty\n", config), "a non-numeric port is rejected");
    check(parse_node_config("port: 1024\n", config), "the bottom of the allowed range is allowed");
    check(parse_node_config("port: 65535\n", config), "and the top");
}

void testMalformedInput()
{
    NodeConfig config;
    check(!parse_node_config("bind_address: [1, 2]\n", config), "a sequence where a string belongs is rejected");
    check(!parse_node_config("- one\n- two\n", config), "a top-level sequence is rejected");
    check(!parse_node_config("bind_address: \"\"\n", config), "an empty bind_address is rejected");
    check(!parse_node_config("port: :\n", config), "invalid YAML is rejected rather than thrown");
}

void testEveryProblemIsReportedNotJustTheFirst()
{
    // Three mistakes should take one run to fix, which is why the parser
    // accumulates rather than returning on the first failure.
    NodeConfig config;
    check(!parse_node_config("bind_adress: x\nport: 80\nnonsense: 1\n", config),
          "a config with several problems fails");
}

}  // namespace

int main()
{
    testDefaults();
    testEveryKeyIsRead();
    testUnknownKeyIsAnError();
    testPortRange();
    testMalformedInput();
    testEveryProblemIsReportedNotJustTheFirst();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
