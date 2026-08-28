// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's YAML, without a radio and without a file.
//
// The cases that matter are the ones that fail QUIETLY if nobody checks them:
// a topic key containing a character zenoh treats specially, which no wildcard
// subscriber would ever see, and a channel-control flag that defaults the
// wrong way -- this node can move somebody's radio, and the default must be
// that it will not.

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

using namespace xpr_node;

void checkDefaults()
{
    NodeConfig config;
    check(parse_node_config("radio:\n  host: 192.168.10.1\n", config), "a minimal config parses");

    check(config.radio.port == 8002, "the plaintext XNL port is the default");
    check(!config.control.allowChannelChange,
          "CHANNEL CONTROL IS OFF BY DEFAULT: this moves somebody's radio");
    check(config.publish.topicPrefix == "nodes/xpr", "topic prefix default");
    // Derived rather than duplicated: a status key that has drifted from the
    // prefix is a topic nobody is looking at.
    check(config.publish.statusKey == "nodes/xpr/status", "the status key follows the prefix");
    check(config.publish.publishDisplay, "the display is mirrored by default");
    check(config.radio.reconnectBackoffMs.size() == 5, "the default backoff schedule survives");
}

void checkFullConfig()
{
    const std::string yaml = R"(
radio:
  host: 10.0.0.7
  port: 8102
  connect_timeout_ms: 500
  reply_timeout_ms: 750
  reconnect_backoff_ms: [100, 200]
control:
  allow_channel_change: true
publish:
  topic_prefix: radios/front
  status_key: radios/front/health
  status_interval_ms: 2000
  publish_display: false
  publish_unknown_broadcasts: false
)";

    NodeConfig config;
    check(parse_node_config(yaml, config), "a full config parses");
    check(config.radio.host == "10.0.0.7", "host");
    check(config.radio.port == 8102, "port");
    check(config.radio.replyTimeoutMs == 750, "reply timeout");
    check(config.radio.reconnectBackoffMs.size() == 2, "the backoff schedule is replaced, not appended");
    check(config.control.allowChannelChange, "channel control can be turned on");
    check(config.publish.statusKey == "radios/front/health", "an explicit status key wins");
    check(!config.publish.publishDisplay, "the display can be turned off");
    check(!config.publish.publishUnknownBroadcasts, "unknown broadcasts can be turned off");
}

void checkRejections()
{
    NodeConfig config;

    check(!parse_node_config("radio:\n  host: ''\n", config), "an empty host is refused");
    check(!parse_node_config("radio:\n  port: 0\n", config), "port zero is refused");
    check(!parse_node_config("radio:\n  port: 99999\n", config), "an out-of-range port is refused");
    check(!parse_node_config("radio:\n  reconnect_backoff_ms: []\n", config),
          "an empty backoff schedule is refused: it would mean retry forever with no pause");
    check(!parse_node_config("publish:\n  status_interval_ms: 0\n", config),
          "a zero status interval is refused");

    // A key with a character zenoh treats specially does not fail on the bus,
    // it just never matches a subscription -- so it is refused here.
    check(!parse_node_config("publish:\n  topic_prefix: 'nodes/xpr*'\n", config),
          "a wildcard in a topic key is refused");
    check(!parse_node_config("publish:\n  topic_prefix: '@nodes/xpr'\n", config),
          "a verbatim-marker segment is refused");

    check(!parse_node_config("radio: [1, 2, 3]\n", config), "a list where a map belongs is refused");
    check(!parse_node_config("radio:\n  port: not-a-number\n", config), "a non-numeric port is refused");
    check(!parse_node_config("radio:\n  host: [oops\n", config), "malformed YAML is refused");
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");
    // The parser logs every problem it finds, and most of this file feeds it
    // problems on purpose.
    spdlog::set_level(spdlog::level::critical);

    checkDefaults();
    checkFullConfig();
    checkRejections();

    spdlog::set_level(spdlog::level::info);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    SPDLOG_INFO("xpr_test_config passed");
    return 0;
}
