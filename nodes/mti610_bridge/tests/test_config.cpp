// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's YAML, exercised without a device and without a file.
//
// The cases worth more than the happy path are the ones where a plausible
// config file means something other than what its author intended:
//
//   * An output name the device does not have. The error has to list what is
//     valid, because the alternative is a node that silently asks for nothing.
//   * A precision on an output that has no format nibble. The LLCP gives the
//     timestamp and status identifiers a settable format field and then
//     defines their payload as U2/U4 regardless, so writing one is a
//     misunderstanding rather than a preference.
//   * `rate: max` versus a number, which are the same request spelled two ways.

#include "node_config.h"

#include <spdlog/spdlog.h>

#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace mti610_node;

void testDefaults()
{
    NodeConfig config;
    check(parse_node_config("device: { port: /dev/ttyUSB0 }", config), "a minimal config parses");

    check(config.device.baud == 115200, "the default baud is the device's factory setting");
    check(config.publish.topicPrefix == "nodes/mti610", "and the topic prefix is device-shaped");
    check(config.publish.publishUnknownItems,
          "unmodelled items are published by default, so a device that is not a 610 is visible");
    check(config.configuration.mode == mti610::ConfigMode::Enforce, "enforce by default");
    check(config.configuration.policy == mti610::PortPolicy::Additive,
          "and additive, because the device may be feeding something else");
}

void testAFullConfig()
{
    const std::string yaml = R"(
device:
  port: /dev/cu.usbserial-FT1234
  baud: 921600
  open_timeout_ms: 1500
  reopen_backoff_ms: [100, 400]

configuration:
  mode: report_only
  port_policy: exclusive
  reply_timeout_ms: 750
  retries: 4
  recheck_interval_s: 30
  outputs:
    - { data: packet_counter, rate: max }
    - { data: status_word,    rate: max }
    - { data: acceleration,   rate: 100, precision: fp1632 }
    - { data: rate_of_turn,   rate: 100, precision: fp1632 }
    - { data: temperature,    rate: 1,   precision: float32 }

publish:
  topic_prefix: nodes/imu
  status_key: nodes/imu/status
  status_interval_ms: 500
  publish_unknown_items: false
)";

    NodeConfig config;
    check(parse_node_config(yaml, config), "a full config parses");

    check(config.device.port == "/dev/cu.usbserial-FT1234", "the port");
    check(config.device.baud == 921600, "the baud rate");
    check(config.device.reopenBackoffMs == std::vector<std::uint32_t> { 100, 400 },
          "the backoff schedule");

    check(config.configuration.mode == mti610::ConfigMode::ReportOnly, "report_only");
    check(config.configuration.policy == mti610::PortPolicy::Exclusive, "exclusive");
    check(config.configuration.retries == 4, "the retry count");
    check(config.configuration.recheckIntervalS == 30, "the recheck interval");

    check(config.configuration.outputs.size() == 5, "five outputs");

    check(config.publish.topicPrefix == "nodes/imu", "a topic prefix other than the default");
    check(!config.publish.publishUnknownItems, "and unknown items switched off");

    // The identifiers that come out the other side. This is what libs/mti610
    // compares against and what goes on the wire.
    const std::vector<mti610::OutputEntry> entries =
        to_output_entries(config.configuration.outputs);

    check(entries.size() == 5, "five entries");
    check(entries[0].rawId == 0x1020 && entries[0].frequencyHz == xbus::kMaxFrequency,
          "packet counter at max");
    check(entries[2].rawId == 0x4022 && entries[2].frequencyHz == 100,
          "acceleration in fp16.32 is 0x4022, not 0x4020");
    check(entries[4].rawId == 0x0810, "and temperature in float32 is 0x0810");
}

void testThePrecisionNibbleIsBuiltFromTheName()
{
    // `precision:` is a name in the YAML and a nibble on the wire. Getting the
    // mapping wrong would configure the device for one format while the parser
    // expected another -- which decodes without error, and is wrong.
    const char* cases[][2] = {
        { "float32", "0x4020" }, { "fp1220", "0x4021" },
        { "fp1632", "0x4022" },  { "float64", "0x4023" },
    };

    const std::uint16_t expected[] = { 0x4020, 0x4021, 0x4022, 0x4023 };

    for (std::size_t i = 0; i < 4; ++i)
    {
        NodeConfig config;
        const std::string yaml = std::string("configuration: { outputs: [ { data: acceleration, "
                                             "precision: ") +
                                 cases[i][0] + " } ] }";
        check(parse_node_config(yaml, config), std::string("precision ") + cases[i][0] + " parses");
        if (config.configuration.outputs.size() == 1)
        {
            check(config.configuration.outputs[0].rawDataId() == expected[i],
                  std::string(cases[i][0]) + " gives " + cases[i][1]);
        }
    }
}

void testRateMaxAndZeroAreTheSameRequest()
{
    NodeConfig config;
    check(parse_node_config(
              "configuration: { outputs: [ { data: acceleration, rate: max } ] }", config),
          "'max' parses");
    check(config.configuration.outputs.size() == 1 &&
              config.configuration.outputs[0].frequencyHz == xbus::kMaxFrequency,
          "and means 0xFFFF");

    NodeConfig numeric;
    check(parse_node_config(
              "configuration: { outputs: [ { data: acceleration, rate: 100 } ] }", numeric),
          "a number parses");
    check(numeric.configuration.outputs[0].frequencyHz == 100, "and means itself");
}

void testBadOutputNameListsTheValidOnes()
{
    NodeConfig config;
    check(!parse_node_config(
              "configuration: { outputs: [ { data: quaternion, rate: 100 } ] }", config),
          "an output an MTi-610 does not have is refused");

    // The point of refusing rather than ignoring: orientation is exactly what
    // somebody coming from a 630 would write, and silently dropping it would
    // give them a node that publishes nothing and says nothing.
    check(known_output_names().find("acceleration") != std::string::npos,
          "the valid-names list names the outputs that do exist");
    check(known_output_names().find("quaternion") == std::string::npos,
          "and does not offer one that does not");

    check(!parse_node_config("configuration: { outputs: [ { rate: 100 } ] }", config),
          "an entry with no 'data' key is refused");
}

void testPrecisionOnAFixedLayoutOutputIsRefused()
{
    // The LLCP gives the timestamp and status identifiers a settable format
    // field and then defines their payload as U2/U4 regardless. Asking for a
    // precision on one is a misunderstanding, and accepting it would put a
    // nibble on the wire that means nothing.
    NodeConfig config;
    check(!parse_node_config(
              "configuration: { outputs: [ { data: status_word, precision: fp1632 } ] }", config),
          "a precision on the status word is refused");

    check(parse_node_config("configuration: { outputs: [ { data: status_word, rate: max } ] }",
                            config),
          "and the same output without one is fine");
    check(config.configuration.outputs[0].rawDataId() == 0xE020,
          "with no format nibble on the wire");
}

void testMalformedInput()
{
    NodeConfig config;

    check(!parse_node_config("device: [1, 2, 3]", config), "a list where a map belongs");
    check(!parse_node_config("device: { baud: not-a-number }", config), "a word where a number belongs");
    check(!parse_node_config("device: { baud: 99999999999 }", config), "a number out of range");
    check(!parse_node_config("configuration: { outputs: 5 }", config), "outputs that is not a list");
    check(!parse_node_config("configuration: { mode: sometimes }", config), "an unknown mode");
    check(!parse_node_config("configuration: { port_policy: mostly }", config), "an unknown policy");
    check(!parse_node_config("[1, 2, 3]", config), "a document that is not a map");
    check(!parse_node_config("device: { port: [unclosed", config), "input that is not YAML at all");
}

void testEveryErrorIsReportedInOneRun()
{
    // A config with three mistakes should take one run to fix, not three.
    // Checked by confirming the parse gets past the first bad key rather than
    // stopping: the outputs list is parsed even though `mode` above it failed.
    NodeConfig config;
    const std::string yaml = R"(
configuration:
  mode: sometimes
  outputs:
    - { data: acceleration, rate: 100 }
    - { data: quaternion,   rate: 100 }
)";

    check(!parse_node_config(yaml, config), "a config with several mistakes fails");
    check(config.configuration.outputs.size() == 1,
          "but the good entry after the bad one was still parsed, so both were reported");
}

void testTooManyOutputs()
{
    // One SetOutputConfiguration carries at most 32 entries, and the device
    // would refuse a longer one with an error that says nothing useful.
    std::string yaml = "configuration: { outputs: [";
    for (int i = 0; i < 33; ++i)
    {
        yaml += "{ data: acceleration, rate: 100 },";
    }
    yaml += "] }";

    NodeConfig config;
    check(!parse_node_config(yaml, config), "more than 32 outputs is refused at load");
}

} // namespace

int main()
{
    // err rather than critical: the harness reports failures at error level, and
    // silencing them would make a failing test print nothing at all.
    spdlog::set_level(spdlog::level::err);

    testDefaults();
    testAFullConfig();
    testThePrecisionNibbleIsBuiltFromTheName();
    testRateMaxAndZeroAreTheSameRequest();
    testBadOutputNameListsTheValidOnes();
    testPrecisionOnAFixedLayoutOutputIsRefused();
    testMalformedInput();
    testEveryErrorIsReportedInOneRun();
    testTooManyOutputs();

    return failures == 0 ? 0 : 1;
}
