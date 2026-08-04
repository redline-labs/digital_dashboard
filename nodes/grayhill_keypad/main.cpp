// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Grayhill 3K keypad, as a running device: buttons out, indicators and
// brightness in.
//
// This node speaks PDO, and confirms the one SDO write it makes. It does not
// configure the keypad -- no COB-IDs, no node ID, no bit rate, nothing that
// touches non-volatile memory. That is grayhill_keypad_reconfigure's job, and
// keeping the two apart means neither has to reason about the other's state.
//
// Four things here were wrong before and are worth naming, because each of them
// looked like it worked:
//
//   * The startup burst went out immediately after the publisher was
//     constructed, while the subscriber was still being declared. Zenoh has no
//     retained messages, so with peering not yet established those frames were
//     simply lost, and nothing noticed.
//   * The heartbeat SDO write was fire-and-forget. A keypad that aborted it
//     was indistinguishable from one that accepted it.
//   * Every received frame that was not TPDO1 was discarded, so heartbeats,
//     boot-ups and emergencies all arrived and were thrown away.
//   * Each brightness service sent RPDO2 with the other channel zeroed, so
//     setting one blanked the other -- and zero is below what the indicator
//     channel accepts, so it was also out of range.

#include "node_config.h"

#include "canopen/nmt.h"
#include "canopen/sdo.h"
#include "canopen/zenoh_bus.h"

#include "canopen_grayhill_helpers.h"
#include "canopen_grayhill_node.h"

#include "grayhill_keypad.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <string>

namespace
{

std::atomic<bool> running { true };

void handle_signal(int)
{
    running = false;
}

// The brightness the keypad was last told to use. RPDO2 carries both channels
// in one frame, so a request about one channel has to say what the other one
// is -- and the only honest answer is "whatever we last sent".
struct Brightness
{
    uint16_t indicator { 255 };
    uint16_t backlight { 0 };
};

GrayhillStatus::State to_schema_state(canopen::NmtState state)
{
    switch (state)
    {
    case canopen::NmtState::BootUp: return GrayhillStatus::State::BOOT_UP;
    case canopen::NmtState::Stopped: return GrayhillStatus::State::STOPPED;
    case canopen::NmtState::Operational: return GrayhillStatus::State::OPERATIONAL;
    case canopen::NmtState::PreOperational: return GrayhillStatus::State::PRE_OPERATIONAL;
    }
    return GrayhillStatus::State::UNKNOWN;
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("grayhill_keypad", "Grayhill 3K CANopen keypad");
    options.add_options()
        ("config", "Node configuration YAML", cxxopts::value<std::string>())
        ("v,verbose", "Enable debug logging")
        ("h,help", "Print usage");

    cxxopts::ParseResult args;
    try
    {
        args = options.parse(argc, argv);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("[node] {}", error.what());
        return 1;
    }

    if (args.count("help") != 0)
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }
    if (args.count("verbose") != 0)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    if (args.count("config") == 0)
    {
        SPDLOG_ERROR("[node] --config is required. Start from "
                     "configs/grayhill_keypad/grayhill_keypad.yaml, which documents every field.");
        return 1;
    }

    grayhill::NodeConfig config;
    if (!grayhill::load_node_config(args["config"].as<std::string>(), config))
    {
        SPDLOG_ERROR("[node] refusing to start with an unusable --config");
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    SPDLOG_INFO("[node] keypad at node {} on '{}' / '{}'", config.nodeId, config.rxKey,
                config.txKey);

    canopen::ZenohBus bus(config.txKey, config.rxKey);
    if (!bus.is_valid())
    {
        SPDLOG_ERROR("[node] could not open a zenoh session");
        return 1;
    }

    canopen::NmtMaster nmt(bus);
    canopen::SdoClient sdo(bus, config.nodeId);
    canopen_grayhill::node device(config.nodeId);

    // The generated class handles the PDOs it knows; everything else -- SDO
    // responses, heartbeats, emergencies -- is picked up by the subscribers
    // that NmtMaster and SdoClient installed on the bus.
    bus.subscribe([&device](const helpers::CanFrame& frame) { (void)device.handle_frame(frame); });

    // --- what we publish ----------------------------------------------------
    pub_sub::ZenohPublisher<GrayhillButtons> buttonsPublisher(config.topicPrefix + "/buttons");
    pub_sub::ZenohPublisher<GrayhillStatus> statusPublisher(config.topicPrefix + "/status");

    uint32_t bootCount = 0;
    uint16_t lastEmergency = 0;
    auto publishStatus = [&](canopen::NmtState state)
    {
        auto& fields = statusPublisher.fields();
        fields.setState(to_schema_state(state));
        fields.setBootCount(bootCount);
        fields.setLastEmergencyCode(lastEmergency);
        statusPublisher.put();
    };

    device.on_tpdo1(
        [&](const canopen_grayhill::Tpdo1& buttons)
        {
            auto& fields = buttonsPublisher.fields();
            fields.setButtons1To8(buttons.digital_input_buttons_1_through_8);
            fields.setButtons9To16(buttons.digital_input_buttons_9_through_16);
            fields.setButtons17To24(buttons.digital_input_buttons_17_through_24);
            buttonsPublisher.put();

            SPDLOG_DEBUG("[keypad] buttons 0x{:02X} 0x{:02X} 0x{:02X}",
                         buttons.digital_input_buttons_1_through_8,
                         buttons.digital_input_buttons_9_through_16,
                         buttons.digital_input_buttons_17_through_24);
        });

    nmt.on_state_change(
        [&](uint8_t nodeId, canopen::NmtState state)
        {
            if (nodeId != config.nodeId)
            {
                return;
            }
            if (state == canopen::NmtState::BootUp)
            {
                ++bootCount;
                // A keypad that reboots on its own is a wiring or supply
                // problem, and it is worth saying so rather than silently
                // re-applying settings.
                SPDLOG_WARN("[keypad] node {} booted (boot #{})", nodeId, bootCount);
            }
            else
            {
                SPDLOG_INFO("[keypad] node {} is {}", nodeId, canopen::to_string(state));
            }
            publishStatus(state);
        });

    nmt.on_emergency(
        [&](const canopen::EmcyMessage& message)
        {
            if (message.nodeId != config.nodeId)
            {
                return;
            }
            lastEmergency = message.errorCode;
            SPDLOG_WARN("[keypad] {}", canopen::to_string(message));
            publishStatus(nmt.state(config.nodeId).value_or(canopen::NmtState::PreOperational));
        });

    // --- let zenoh settle before saying anything ----------------------------
    //
    // Everything above declares a publisher or a subscriber. Sending before
    // peering is established loses the frames, so the startup sequence waits
    // rather than racing.
    for (uint32_t elapsed = 0; elapsed < config.startupDelayMs && running; elapsed += 20)
    {
        bus.poll(canopen::Duration { 20 });
    }

    Brightness brightness { config.indicatorBrightness, config.backlightBrightness };

    if (config.driveNmt && running)
    {
        nmt.command(canopen::NmtCommand::EnterPreOperational, config.nodeId);
        bus.poll(canopen::Duration { 50 });

        if (config.heartbeatMs != 0)
        {
            // Confirmed, unlike before. A keypad that refuses this is a keypad
            // whose state we will never hear about, which is worth a warning
            // rather than silence.
            auto result = sdo.download_u16(0x1017, 0, config.heartbeatMs);
            if (result.has_value())
            {
                SPDLOG_INFO("[keypad] producer heartbeat set to {} ms", config.heartbeatMs);
            }
            else
            {
                SPDLOG_WARN("[keypad] could not set the producer heartbeat: {}",
                            canopen::to_string(result.error()));
                SPDLOG_WARN("[keypad] the node will run without heartbeat tracking");
            }
        }

        nmt.command(canopen::NmtCommand::Start, config.nodeId);
        bus.poll(canopen::Duration { 50 });
        SPDLOG_INFO("[keypad] node {} taken to operational", config.nodeId);
    }

    // Apply the configured brightness once, now that the keypad is running.
    {
        canopen_grayhill::Rpdo2 frame {};
        frame.analog_output_indicator_brightness = static_cast<int16_t>(brightness.indicator);
        frame.analog_output_backlight_brightness = static_cast<int16_t>(brightness.backlight);
        bus.send(device.make_rpdo2(frame));
    }

    // --- services -----------------------------------------------------------
    //
    // Both brightness services send the whole frame, with the channel they are
    // not about carried over from what was last sent. Sending zero for it --
    // which is what these used to do -- blanked the other channel, and for the
    // indicator channel zero is below the device's minimum of 1.
    auto sendBrightness = [&](uint16_t indicator, uint16_t backlight) -> std::string
    {
        if (indicator < 1 || indicator > 255)
        {
            return fmt::format("indicator brightness {} is outside the device's range 1..255",
                               indicator);
        }
        if (backlight > 255)
        {
            return fmt::format("backlight brightness {} is outside the device's range 0..255",
                               backlight);
        }

        canopen_grayhill::Rpdo2 frame {};
        frame.analog_output_indicator_brightness = static_cast<int16_t>(indicator);
        frame.analog_output_backlight_brightness = static_cast<int16_t>(backlight);
        bus.send(device.make_rpdo2(frame));

        brightness.indicator = indicator;
        brightness.backlight = backlight;
        return {};
    };

    pub_sub::ZenohService<GrayhillSetIndicatorBrightnessRequest,
                          GrayhillSetIndicatorBrightnessResponse>
        indicatorService(
            config.topicPrefix + "/set_indicator_brightness",
            [&](const GrayhillSetIndicatorBrightnessRequest::Reader& request,
                GrayhillSetIndicatorBrightnessResponse::Builder& response)
            {
                const uint16_t backlight = request.getBacklight() == OtherChannel::ZERO
                    ? 0
                    : brightness.backlight;
                const std::string error = sendBrightness(request.getValue(), backlight);
                response.setOk(error.empty());
                response.setError(error);
                if (!error.empty())
                {
                    SPDLOG_WARN("[keypad] {}", error);
                }
            });

    pub_sub::ZenohService<GrayhillSetBacklightBrightnessRequest,
                          GrayhillSetBacklightBrightnessResponse>
        backlightService(
            config.topicPrefix + "/set_backlight_brightness",
            [&](const GrayhillSetBacklightBrightnessRequest::Reader& request,
                GrayhillSetBacklightBrightnessResponse::Builder& response)
            {
                // `zero` is not offered for the indicator channel because the
                // device would abort it; the request can ask, and gets told.
                const uint16_t indicator = request.getIndicator() == OtherChannel::ZERO
                    ? 0
                    : brightness.indicator;
                const std::string error = sendBrightness(indicator, request.getValue());
                response.setOk(error.empty());
                response.setError(error);
                if (!error.empty())
                {
                    SPDLOG_WARN("[keypad] {}", error);
                }
            });

    pub_sub::ZenohService<GrayhillSetIndicatorsRequest, GrayhillSetIndicatorsResponse>
        indicatorsService(
            config.topicPrefix + "/set_indicators",
            [&](const GrayhillSetIndicatorsRequest::Reader& request,
                GrayhillSetIndicatorsResponse::Builder& response)
            {
                auto bytes = request.getIndicators();
                if (bytes.size() > canopen_grayhill::RPDO1_LENGTH)
                {
                    const std::string error
                        = fmt::format("{} indicator bytes given; RPDO1 carries {}", bytes.size(),
                                      canopen_grayhill::RPDO1_LENGTH);
                    response.setOk(false);
                    response.setError(error);
                    SPDLOG_WARN("[keypad] {}", error);
                    return;
                }

                // The generated struct has a named field per mapped byte, so
                // filling it from a list means going through the frame the
                // mapping describes rather than assuming a layout.
                canopen_grayhill::Rpdo1 indicators {};
                auto frame = canopen_grayhill::pack_rpdo1(indicators, config.nodeId);
                for (unsigned i = 0; i < bytes.size(); ++i)
                {
                    frame.data[i] = bytes[i];
                }
                bus.send(frame);

                response.setOk(true);
                response.setError("");
            });

    SPDLOG_INFO("[node] running; publishing buttons on '{}/buttons'", config.topicPrefix);
    publishStatus(nmt.state(config.nodeId).value_or(canopen::NmtState::PreOperational));

    while (running)
    {
        bus.poll(canopen::Duration { 50 });
    }

    // --- shutdown -----------------------------------------------------------
    //
    // Leaving the keypad operational after this process exits means it keeps
    // transmitting button state at nobody, and its indicators keep whatever
    // they were last told. Stopping it is the honest end state.
    SPDLOG_INFO("[node] shutting down");
    if (config.driveNmt)
    {
        canopen_grayhill::Rpdo1 dark {};
        bus.send(device.make_rpdo1(dark));

        nmt.command(canopen::NmtCommand::EnterPreOperational, config.nodeId);
        bus.poll(canopen::Duration { 50 });
    }

    return 0;
}
