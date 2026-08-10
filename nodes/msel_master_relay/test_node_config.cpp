// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MSEL node's config file, and the two ways it can be quietly wrong.
//
// The first is a typo. A misspelled key that is ignored looks exactly like a
// setting that does not work, and on this node the setting most worth getting
// right -- the base address -- produces no error at all when it is wrong: the
// relay simply never appears. So unknown keys are rejected, and every address
// is range-checked here rather than discovered on a car.
//
// The second is the remote shutdown gate. `allow_can_kill` decides whether this
// node is willing to isolate a battery and stop an engine, so its default and
// its parsing are worth a test of their own.

#include "node_config.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

// Most of this file feeds the loader configs that are wrong on purpose, and the
// loader logs every problem it finds. Logging is therefore switched off while
// the checks run, so failures are collected here and reported at the end rather
// than being lost in the noise they are surrounded by.
std::vector<std::string> failures;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        failures.push_back(what);
    }
}

msel_node::NodeConfig parseOk(const std::string& yaml, const std::string& what)
{
    msel_node::NodeConfig config;
    if (!msel_node::parse_node_config(yaml, config))
    {
        failures.push_back(what + ": was rejected and should have parsed");
    }
    return config;
}

void parseFails(const std::string& yaml, const std::string& what)
{
    msel_node::NodeConfig config;
    if (msel_node::parse_node_config(yaml, config))
    {
        failures.push_back(what + ": was accepted and should not have been");
    }
}

void test_defaults()
{
    const auto config = parseOk("rx_key: vehicle/can0/rx\n", "a minimal config");

    check(config.rxKey == "vehicle/can0/rx", "rx_key is read");
    check(config.txKey == "vehicle/can0/tx", "tx_key defaults to the matching bridge topic");
    check(config.topicPrefix == "nodes/msel_master_relay", "topic_prefix has a default");
    check(config.baseAddress == 0x6E4u, "base_address defaults to the factory address");
    check(config.killAddress == 0x6E6u, "kill_address has a default");

    // The one default that matters more than the others.
    check(!config.allowCanKill,
          "remote shutdown is off unless it is explicitly turned on");
}

void test_addresses_are_read_as_hex()
{
    // CAN identifiers are written in hex in the manual, on the bus, and in every
    // other tool. A config that read 0x6E4 as zero would be a trap.
    const auto hex = parseOk("base_address: 0x500\nkill_address: 0x123\n", "hex addresses");
    check(hex.baseAddress == 0x500u, "0x500 parses as 1280, not as 0");
    check(hex.killAddress == 0x123u, "0x123 parses as 291");

    const auto decimal = parseOk("base_address: 1280\n", "decimal addresses");
    check(decimal.baseAddress == 1280u, "a decimal address still works");

    parseFails("base_address: not-a-number\n", "a base address that is not a number");
    parseFails("base_address: 0xZZ\n", "a base address with bad hex digits");
}

void test_unknown_keys_are_rejected()
{
    parseFails("bass_address: 0x500\n", "a misspelled base_address");
    parseFails("allow_can_kil: true\n", "a misspelled allow_can_kill");
    parseFails("rx_key: a/b\nnonsense: 1\n", "an unrecognised key alongside valid ones");
}

void test_addresses_are_validated()
{
    // base+3 has to fit in 11 bits, not just base.
    parseFails("base_address: 0x7FF\n", "a base address whose third message overflows");
    parseFails("base_address: 0x800\n", "a base address wider than 11 bits");
    check(parseOk("base_address: 0x7FC\nkill_address: 0x100\n", "the largest base that fits")
              .baseAddress == 0x7FCu,
          "0x7FC is accepted");

    // A base spanning 0x789 would leave the relay unable to be reconfigured.
    parseFails("base_address: 0x789\n", "a base address on the configuration identifier");
    parseFails("base_address: 0x787\n", "a base address whose span covers 0x789");

    parseFails("kill_address: 0x800\n", "a kill address wider than 11 bits");
    parseFails("kill_address: 0x789\n", "a kill address on the configuration identifier");

    // A kill address on one of the three identifiers the relay transmits would
    // have the node writing onto something it also decodes as telemetry.
    parseFails("base_address: 0x6E4\nkill_address: 0x6E4\n",
               "a kill address on the status message");
    parseFails("base_address: 0x6E4\nkill_address: 0x6E5\n",
               "a kill address on the info message");
    parseFails("base_address: 0x6E4\nkill_address: 0x6E7\n",
               "a kill address on the switch-state message");

    // But base+2 must be accepted, and this is the case worth pinning: the
    // relay transmits on base, base+1 and base+3 only, and the gap at base+2 is
    // exactly where MSEL's own documentation puts the kill frame. A rule that
    // banned the whole base..base+3 span would refuse the factory defaults.
    check(parseOk("base_address: 0x6E4\nkill_address: 0x6E6\n", "the vendor's default pairing")
              .killAddress == 0x6E6u,
          "0x6E6 is base+2, which the relay never transmits on, and must be accepted");
    check(parseOk("base_address: 0x500\nkill_address: 0x502\n", "base+2 after re-addressing")
              .killAddress == 0x502u,
          "base+2 is still free once the relay has been moved");
}

void test_topic_keys_are_validated()
{
    parseFails("rx_key: ''\n", "an empty rx_key");
    parseFails("topic_prefix: 'has spaces'\n", "a topic prefix that is not a legal key");

    // Feeding this node's own commands back into its decoder would have it
    // decode 0x789 traffic it had just sent.
    parseFails("rx_key: vehicle/can0/rx\ntx_key: vehicle/can0/rx\n",
               "rx_key and tx_key pointing at the same topic");
}

void test_can_kill_gate()
{
    check(parseOk("allow_can_kill: true\n", "remote shutdown permitted").allowCanKill,
          "allow_can_kill: true is read");
    check(!parseOk("allow_can_kill: false\n", "remote shutdown refused").allowCanKill,
          "allow_can_kill: false is read");
    parseFails("allow_can_kill: maybe\n", "allow_can_kill that is not a boolean");
}

void test_malformed_files()
{
    parseFails("", "an empty file");
    parseFails("- a\n- b\n", "a file that is a list rather than a mapping");
    parseFails("rx_key: [unclosed\n", "a file that is not valid YAML");
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    // Silence the loader while it is being fed bad input on purpose. Failures
    // are collected rather than logged, and printed below once logging is back.
    spdlog::set_level(spdlog::level::off);

    test_defaults();
    test_addresses_are_read_as_hex();
    test_unknown_keys_are_rejected();
    test_addresses_are_validated();
    test_topic_keys_are_validated();
    test_can_kill_gate();
    test_malformed_files();

    spdlog::set_level(spdlog::level::info);
    if (!failures.empty())
    {
        for (const auto& failure : failures)
        {
            SPDLOG_ERROR("FAIL: {}", failure);
        }
        SPDLOG_ERROR("{} check(s) failed", failures.size());
        return 1;
    }

    SPDLOG_INFO("all MSEL node config checks passed");
    return 0;
}
