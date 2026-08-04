// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bridge's config surface.
//
// Most of what is checked here is refusal. A CAN bridge's config decides which
// physical adapter gets opened and which topic its traffic appears on, and the
// mistakes that matter are the ones that produce a running node doing the wrong
// thing quietly: two channels racing for one dongle, two buses interleaved into
// one topic, a misspelled key that reverts a setting to its default.

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

bool parses(const std::string& yaml, can_bridge::NodeConfig& out)
{
    return can_bridge::parse_node_config(yaml, out);
}

void test_minimal_config()
{
    can_bridge::NodeConfig config;
    const bool ok = parses(R"(
channels:
  - name: can0
    device: "virtual:bench"
    bitrate: 500000
)",
                           config);

    check(ok, "a one-channel config parses");
    if (!ok)
    {
        return;
    }

    check(config.channels.size() == 1, "with one channel");
    const auto& channel = config.channels[0];
    check(channel.name == "can0", "named can0");
    check(channel.device == "virtual:bench", "on the virtual bus");
    check(channel.bitrateBps == 500000, "at 500 kbit/s");

    // The keys default from the name, which is what keeps the common case to
    // three lines per bus.
    check(channel.rxKey == "vehicle/can0/rx", "with rx defaulted from the name");
    check(channel.txKey == "vehicle/can0/tx", "and tx too");

    check(channel.dataBitrateBps == 0, "classic CAN unless a data rate is asked for");
    check(!channel.listenOnly, "transmitting by default");
    check(channel.publishRx && channel.acceptTx, "and bridging both directions");
}

void test_multi_channel_one_dongle()
{
    // The case the whole design exists for: two CAN channels on one PCAN
    // adapter, in one process, because they share a USB handle that cannot be
    // opened twice.
    can_bridge::NodeConfig config;
    const bool ok = parses(R"(
channels:
  - name: powertrain
    device: "pcan:0"
    bitrate: 500000
  - name: chassis
    device: "pcan:0/1"
    bitrate: 250000
    listen_only: true
)",
                           config);

    check(ok, "two channels on one adapter parse");
    if (!ok)
    {
        return;
    }

    check(config.channels.size() == 2, "as two channels");
    check(config.channels[0].device == "pcan:0" && config.channels[1].device == "pcan:0/1",
          "distinguished by the channel suffix rather than by the device");
    check(config.channels[0].rxKey == "vehicle/powertrain/rx"
              && config.channels[1].rxKey == "vehicle/chassis/rx",
          "each with its own topics, named after the bus rather than the hardware");
    check(config.channels[1].listenOnly, "and their own settings");
}

void test_fd_config()
{
    can_bridge::NodeConfig config;
    const bool ok = parses(R"(
channels:
  - name: can0
    device: "pcan:0"
    bitrate: 500000
    data_bitrate: 2000000
    sample_point_permille: 800
)",
                           config);

    check(ok, "a CAN FD config parses");
    if (ok)
    {
        check(config.channels[0].dataBitrateBps == 2000000, "with a data-phase rate");
        check(config.channels[0].samplePointPermille == 800, "and an explicit sample point");
    }
}

void test_channels_are_required()
{
    can_bridge::NodeConfig config;
    check(!parses("status_key: \"vehicle/can/status\"\n", config),
          "a config with no channels is refused -- a bridge with nothing to bridge is a "
          "process that looks healthy and does nothing");
    check(!parses("channels: []\n", config), "so is an empty channel list");
    check(!parses("channels:\n  - name: can0\n", config),
          "and a channel with no device");
    check(!parses("channels:\n  - device: \"virtual:a\"\n", config),
          "and one with no name");
}

void test_duplicate_detection()
{
    can_bridge::NodeConfig config;

    // Two channels with the same name would make the bitrate service
    // ambiguous: it addresses a channel by name.
    check(!parses(R"(
channels:
  - name: can0
    device: "virtual:a"
  - name: can0
    device: "virtual:b"
)",
                  config),
          "two channels with the same name are refused");

    // Two channels on the same hardware would fight over it.
    config = {};
    check(!parses(R"(
channels:
  - name: a
    device: "pcan:0/1"
  - name: b
    device: "pcan:0/1"
)",
                  config),
          "two channels on the same device are refused");

    // Two buses published to one key would interleave into a topic nothing can
    // separate again -- and it would look like it was working.
    config = {};
    check(!parses(R"(
channels:
  - name: a
    device: "virtual:a"
    rx_key: "vehicle/merged/rx"
  - name: b
    device: "virtual:b"
    rx_key: "vehicle/merged/rx"
)",
                  config),
          "two channels publishing to the same rx key are refused");

    // A virtual bus is the exception, and deliberately so: two channels on one
    // name is how a loopback pair is built, which is the only way to exercise
    // the receive path with no adapter attached.
    config = {};
    check(parses(R"(
channels:
  - name: sideA
    device: "virtual:pair"
  - name: sideB
    device: "virtual:pair"
)",
                 config),
          "two channels on the same virtual bus are allowed -- sharing one is what it is for");

    // But two channels that both have publishing turned off do not collide,
    // because neither publishes.
    config = {};
    check(parses(R"(
channels:
  - name: a
    device: "virtual:a"
    rx_key: "vehicle/merged/rx"
    publish_rx: false
  - name: b
    device: "virtual:b"
    rx_key: "vehicle/merged/rx"
    publish_rx: false
)",
                 config),
          "unless neither of them publishes");
}

void test_bad_values()
{
    can_bridge::NodeConfig config;

    check(!parses(R"(
channels:
  - name: can0
    device: "virtual:a"
    bitrate: 0
)",
                  config),
          "a zero bit rate is refused");

    config = {};
    check(!parses(R"(
channels:
  - name: can0
    device: "can0"
)",
                  config),
          "a device with no backend prefix is refused -- 'can0' does not say whether that is a "
          "kernel interface or a dongle");

    config = {};
    check(!parses(R"(
channels:
  - name: can0
    device: "virtual:a"
    bitrate: notanumber
)",
                  config),
          "a non-numeric bit rate is refused");

    config = {};
    check(!parses(R"(
channels:
  - name: can0
    device: "virtual:a"
    bitrat: 500000
)",
                  config),
          "a misspelled key is refused rather than ignored, because ignoring it would silently "
          "leave the bus at the default rate");

    config = {};
    check(!parses("channels: not-a-list\n", config), "channels has to be a list");
    check(!parses("", config), "an empty file is refused");
    check(!parses("[1, 2, 3]\n", config), "so is one that is not a mapping");
}

void test_top_level_settings()
{
    can_bridge::NodeConfig config;
    const bool ok = parses(R"(
status_key: "vehicle/buses/status"
set_bitrate_key: "vehicle/buses/set_bitrate"
status_interval_ms: 250
pcan_detach_kernel_driver: true
continue_on_channel_error: false

channels:
  - name: can0
    device: "virtual:a"
)",
                           config);

    check(ok, "the top-level settings parse");
    if (ok)
    {
        check(config.statusKey == "vehicle/buses/status", "the status key is overridable");
        check(config.statusIntervalMs == 250, "so is the status interval");
        check(config.pcanDetachKernelDriver,
              "and detaching the Linux kernel driver, which is off unless asked for because it "
              "removes the socketcan interface that driver created");
        check(!config.continueOnChannelError, "and whether one bad channel stops the node");
    }

    can_bridge::NodeConfig defaults;
    check(parses("channels:\n  - name: a\n    device: \"virtual:a\"\n", defaults),
          "a config without them parses");
    check(defaults.statusKey == "vehicle/can/status", "with a sensible status key");
    check(!defaults.pcanDetachKernelDriver,
          "and without detaching anything from the kernel by default");
    check(defaults.continueOnChannelError,
          "and carrying on past a channel that will not open, so one unplugged adapter does "
          "not take a second bus down with it");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::critical);
    spdlog::set_pattern("[%^%l%$] %v");

    test_minimal_config();
    test_multi_channel_one_dongle();
    test_fd_config();
    test_channels_are_required();
    test_duplicate_detection();
    test_bad_values();
    test_top_level_settings();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all can_bridge config checks passed");
    return 0;
}
