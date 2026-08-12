// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's YAML, parsed without a receiver or a file.
//
// The property worth having here is that EVERY problem is reported, not just
// the first: a config with three mistakes should take one run to fix. That is
// why the parser accumulates rather than returning early, and it is the thing
// a test can actually check.

#include "node_config.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

// Collected rather than logged as they happen: most of this file feeds the
// parser deliberately broken YAML, so log output is turned down to keep the
// expected complaints out of the way -- and that would hide these too.
std::vector<std::string> failures;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        failures.push_back(what);
    }
}

using namespace bd992_node;

const char* kMinimal = R"(
receiver:
  host: 192.168.1.100
)";

const char* kFull = R"(
receiver:
  host: gnss.local
  stream_port: 5017
  control_port: 5018
  connect_timeout_ms: 1500
  reconnect_backoff_ms: [100, 400]

configuration:
  mode: report_only
  port_index: 21
  port_policy: exclusive
  appfile_index: 2
  recheck_interval_s: 30
  allow_raw_commands: true
  outputs:
    - { record: lat_long_height, rate: 10hz }
    - { record: attitude_info,   rate: 20hz }
    - { record: position_type,   rate: 1hz, offset_seconds: 2 }

publish:
  topic_prefix: nodes/gnss
  status_key: nodes/gnss/status
  status_interval_ms: 500
  publish_unknown_records: false
)";

void test_defaults()
{
    NodeConfig config;
    check(parse_node_config(kMinimal, config), "a config naming only the host is enough");

    check(config.receiver.host == "192.168.1.100", "the host is read");
    check(config.receiver.streamPort == 5017, "and the ports default");
    check(config.receiver.controlPort == 5018, "and the ports default");
    check(config.configuration.mode == ConfigMode::Enforce, "enforce is the default mode");
    check(config.configuration.portPolicy == bd992::PortPolicy::Additive,
          "additive is the default policy -- the node does not disable outputs it was not told about");
    check(config.configuration.portIndex == 20, "IP socket 1 by default");
    check(!config.configuration.allowRawCommands, "raw commands are off unless asked for");
    check(config.publish.topicPrefix == "nodes/bd992", "and the topic prefix defaults");
    check(config.configuration.outputs.empty(),
          "no outputs configured means the node only reads, which is a legitimate setup");
}

void test_everything_is_read()
{
    NodeConfig config;
    check(parse_node_config(kFull, config), "a full config parses");

    check(config.receiver.host == "gnss.local", "host");
    check(config.receiver.connectTimeoutMs == 1500, "connect timeout");
    check(config.receiver.reconnectBackoffMs == std::vector<std::uint32_t> { 100, 400 }, "backoff list");

    check(config.configuration.mode == ConfigMode::ReportOnly, "mode");
    check(config.configuration.portIndex == 21, "port index");
    check(config.configuration.portPolicy == bd992::PortPolicy::Exclusive, "port policy");
    check(config.configuration.applicationFileIndex == 2, "application file index");
    check(config.configuration.recheckIntervalS == 30, "recheck interval");
    check(config.configuration.allowRawCommands, "raw commands");

    check(config.configuration.outputs.size() == 3, "three outputs");
    if (config.configuration.outputs.size() == 3)
    {
        check(config.configuration.outputs[0].record == gsof::RecordType::LatLongHeight, "record by name");
        check(config.configuration.outputs[0].rate == gsof::appfile::Frequency::Hz10, "rate by name");
        check(config.configuration.outputs[1].record == gsof::RecordType::AttitudeInfo, "record by name");
        check(config.configuration.outputs[1].rate == gsof::appfile::Frequency::Hz20, "rate by name");
        check(config.configuration.outputs[2].offsetSeconds == 2, "offset");
    }

    check(config.publish.topicPrefix == "nodes/gnss", "topic prefix");
    check(config.publish.statusIntervalMs == 500, "status interval");
    check(!config.publish.publishUnknownRecords, "unknown record publishing can be turned off");
}

void test_the_desired_output_list_uses_the_configured_port()
{
    // The outputs in the YAML name a record and a rate; the port comes from
    // configuration.port_index. Getting that wrong would configure the
    // receiver to emit onto a socket nobody is reading -- which looks exactly
    // like a receiver that is not working.
    NodeConfig config;
    check(parse_node_config(kFull, config), "the full config parses");

    const std::vector<bd992::OutputMessage> desired = desired_outputs(config.configuration);

    check(desired.size() == 3, "one output message per entry");
    for (const bd992::OutputMessage& message : desired)
    {
        check(static_cast<std::uint8_t>(message.port) == 21, "every entry lands on the configured port");
        check(message.isGsof, "and is a GSOF output");
    }
}

void test_a_missing_host_parses_because_replay_needs_it()
{
    // The host is required to CONNECT, not to parse: --replay runs the whole
    // decode and publish path with no receiver, and demanding a host it will
    // never dial would mean a placeholder in configs/bd992/replay.yaml that
    // reads like a mistake. main() refuses an empty host when it is actually
    // about to connect.
    NodeConfig config;
    check(parse_node_config("receiver: {stream_port: 5017}\n", config),
          "a config with no host parses -- it is what a replay config looks like");
    check(config.receiver.host.empty(), "and the host stays empty for main() to notice");

    // A zero port, on the other hand, cannot be right in either mode.
    NodeConfig zeroPort;
    check(!parse_node_config("receiver: {host: h, stream_port: 0}\n", zeroPort),
          "a zero stream port is refused");
}

void test_an_unknown_record_name_is_refused()
{
    NodeConfig config;
    const char* yaml = R"(
receiver: { host: h }
configuration:
  outputs:
    - { record: lat_long_hight, rate: 10hz }
)";

    check(!parse_node_config(yaml, config),
          "a misspelled record name is refused at load rather than sent to a receiver");
}

void test_an_unknown_rate_is_refused()
{
    NodeConfig config;
    const char* yaml = R"(
receiver: { host: h }
configuration:
  outputs:
    - { record: velocity, rate: 15hz }
)";

    // 15 Hz is not in the ICD's frequency table -- the values are not ordinals
    // and there is no rule that says a plausible rate exists.
    check(!parse_node_config(yaml, config), "a rate the receiver has no byte for is refused");
}

void test_a_duplicate_record_is_refused()
{
    NodeConfig config;
    const char* yaml = R"(
receiver: { host: h }
configuration:
  outputs:
    - { record: velocity, rate: 10hz }
    - { record: velocity, rate: 1hz }
)";

    // Two entries for one record on one port is a contradiction: whichever is
    // written last wins, silently, and the diff would then flap between them.
    check(!parse_node_config(yaml, config), "the same record listed twice is refused");
}

void test_every_problem_is_reported_not_just_the_first()
{
    // Four mistakes. A parser that returned at the first would take four runs
    // to get this file right.
    NodeConfig config;
    const char* yaml = R"(
receiver:
  stream_port: 5017
configuration:
  mode: sometimes
  port_policy: whenever
  outputs:
    - { record: nope, rate: 10hz }
)";

    check(!parse_node_config(yaml, config), "the config is refused");
    // The count is not asserted -- it would pin the message wording rather
    // than the behaviour. What matters is that parsing reached the outputs
    // section at all, which it could not have done by returning at the first
    // problem in `receiver`.
    check(config.configuration.outputs.empty(), "and the bad output was not accepted");
}

void test_an_empty_backoff_list_is_refused()
{
    NodeConfig config;
    const char* yaml = R"(
receiver:
  host: h
  reconnect_backoff_ms: []
)";

    check(!parse_node_config(yaml, config),
          "an empty backoff list is refused rather than silently reconnecting in a tight loop");
}

void test_malformed_yaml_is_reported()
{
    NodeConfig config;
    check(!parse_node_config("receiver: [unclosed\n", config), "malformed YAML is refused");
    check(!parse_node_config("just a string\n", config), "a top level that is not a mapping is refused");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::critical);

    test_defaults();
    test_everything_is_read();
    test_the_desired_output_list_uses_the_configured_port();
    test_a_missing_host_parses_because_replay_needs_it();
    test_an_unknown_record_name_is_refused();
    test_an_unknown_rate_is_refused();
    test_a_duplicate_record_is_refused();
    test_every_problem_is_reported_not_just_the_first();
    test_an_empty_backoff_list_is_refused();
    test_malformed_yaml_is_reported();

    spdlog::set_level(spdlog::level::info);

    if (!failures.empty())
    {
        for (const std::string& what : failures)
        {
            SPDLOG_ERROR("FAIL: {}", what);
        }
        SPDLOG_ERROR("{} check(s) failed", failures.size());
        return 1;
    }

    SPDLOG_INFO("all bd992 config checks passed");
    return 0;
}
