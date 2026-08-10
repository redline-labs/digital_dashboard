// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bridges an MSEL Master Relay between the raw CAN topics and the rest of the
// bus: telemetry out as typed messages, settings in as services.
//
// The node is deliberately thin. Everything that knows the protocol lives in
// libs/msel, which never touches zenoh, so the interesting behaviour is
// testable against a stub relay with nothing plugged in. What is left here is
// translation and policy.
//
// The policy worth knowing about:
//
//   - Writes are single-shot. A service call builds one frame, publishes it to
//     the CAN tx topic, and returns. It does not wait for the relay to answer,
//     because the relay only answers if a human was holding the external kill
//     switch when the frame arrived, and blocking a service call on a physical
//     act would just turn every mistimed attempt into a timeout. The answer,
//     when it comes, is published on `<prefix>/config_response`.
//
//   - The response to a command arrives on the relay's *base status
//     identifier*, sharing an id with the periodic status message. Telling the
//     two apart is msel::decodeConfigResponse's job; see the comment there for
//     why it is safe.
//
//   - Remote shutdown is refused unless the config permits it and the request
//     carries the confirmation token. Both, not either.
//
//   - Changing the base address retunes the decoder in place, so telemetry
//     keeps flowing without a restart. Nothing else on the bus knows, which is
//     why it is logged loudly.

#include "node_config.h"

#include "msel/decoder.h"
#include "msel/protocol.h"

#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_service.h"
#include "pub_sub/zenoh_subscriber.h"

#include "can_frame.capnp.h"
#include "msel_master_relay.capnp.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> gRunning { true };

void onSignal(int)
{
    gRunning = false;
}

// --- enum translation -------------------------------------------------------
//
// The library's enums and the schema's enums are separate on purpose: the
// schema has to carry an `unknown` for values the device may report and this
// build does not recognise, and the library has to be able to say "absent"
// without inventing an enumerator. Every switch below names all of its cases,
// so adding a value to either side is a compile error here rather than a
// silently mistranslated setting.

MselRelayStatus toSchema(const std::optional<msel::Status>& status)
{
    if (!status)
    {
        return MselRelayStatus::UNKNOWN;
    }

    switch (*status)
    {
    case msel::Status::Normal: return MselRelayStatus::NORMAL;
    case msel::Status::OverTemperatureWarning: return MselRelayStatus::OVER_TEMPERATURE_WARNING;
    case msel::Status::OverCurrentWarning: return MselRelayStatus::OVER_CURRENT_WARNING;
    case msel::Status::LowVoltageWarning: return MselRelayStatus::LOW_VOLTAGE_WARNING;
    case msel::Status::HighVoltageWarning: return MselRelayStatus::HIGH_VOLTAGE_WARNING;
    case msel::Status::OverTemperatureKill: return MselRelayStatus::OVER_TEMPERATURE_KILL;
    case msel::Status::DriverSwitchKill: return MselRelayStatus::DRIVER_SWITCH_KILL;
    case msel::Status::ExternalSwitchKill: return MselRelayStatus::EXTERNAL_SWITCH_KILL;
    case msel::Status::CanTriggerKill: return MselRelayStatus::CAN_TRIGGER_KILL;
    case msel::Status::PowerOnReset: return MselRelayStatus::POWER_ON_RESET;
    }
    return MselRelayStatus::UNKNOWN;
}

MselCanBaud toSchema(const std::optional<msel::CanBaud>& baud)
{
    if (!baud)
    {
        return MselCanBaud::UNKNOWN;
    }

    switch (*baud)
    {
    case msel::CanBaud::Rate1M: return MselCanBaud::RATE1_MBPS;
    case msel::CanBaud::Rate500k: return MselCanBaud::RATE500_KBPS;
    case msel::CanBaud::Rate250k: return MselCanBaud::RATE250_KBPS;
    }
    return MselCanBaud::UNKNOWN;
}

std::optional<msel::CanBaud> fromSchema(MselCanBaud baud)
{
    switch (baud)
    {
    case MselCanBaud::RATE1_MBPS: return msel::CanBaud::Rate1M;
    case MselCanBaud::RATE500_KBPS: return msel::CanBaud::Rate500k;
    case MselCanBaud::RATE250_KBPS: return msel::CanBaud::Rate250k;
    case MselCanBaud::UNKNOWN: return std::nullopt;
    }
    return std::nullopt;
}

MselOutputDrive toSchema(const std::optional<msel::OutputDrive>& drive)
{
    if (!drive)
    {
        return MselOutputDrive::UNKNOWN;
    }

    switch (*drive)
    {
    case msel::OutputDrive::ActiveHighHalfBridge: return MselOutputDrive::ACTIVE_HIGH_HALF_BRIDGE;
    case msel::OutputDrive::ActiveHighHighSide: return MselOutputDrive::ACTIVE_HIGH_HIGH_SIDE;
    case msel::OutputDrive::ActiveHighLowSide: return MselOutputDrive::ACTIVE_HIGH_LOW_SIDE;
    case msel::OutputDrive::ActiveLowHalfBridge: return MselOutputDrive::ACTIVE_LOW_HALF_BRIDGE;
    case msel::OutputDrive::ActiveLowHighSide: return MselOutputDrive::ACTIVE_LOW_HIGH_SIDE;
    case msel::OutputDrive::ActiveLowLowSide: return MselOutputDrive::ACTIVE_LOW_LOW_SIDE;
    }
    return MselOutputDrive::UNKNOWN;
}

std::optional<msel::OutputDrive> fromSchema(MselOutputDrive drive)
{
    switch (drive)
    {
    case MselOutputDrive::ACTIVE_HIGH_HALF_BRIDGE: return msel::OutputDrive::ActiveHighHalfBridge;
    case MselOutputDrive::ACTIVE_HIGH_HIGH_SIDE: return msel::OutputDrive::ActiveHighHighSide;
    case MselOutputDrive::ACTIVE_HIGH_LOW_SIDE: return msel::OutputDrive::ActiveHighLowSide;
    case MselOutputDrive::ACTIVE_LOW_HALF_BRIDGE: return msel::OutputDrive::ActiveLowHalfBridge;
    case MselOutputDrive::ACTIVE_LOW_HIGH_SIDE: return msel::OutputDrive::ActiveLowHighSide;
    case MselOutputDrive::ACTIVE_LOW_LOW_SIDE: return msel::OutputDrive::ActiveLowLowSide;
    case MselOutputDrive::UNKNOWN: return std::nullopt;
    }
    return std::nullopt;
}

MselCanKillMode toSchema(const std::optional<msel::CanKillMode>& mode)
{
    if (!mode)
    {
        return MselCanKillMode::UNKNOWN;
    }

    switch (*mode)
    {
    case msel::CanKillMode::Disabled: return MselCanKillMode::DISABLED;
    case msel::CanKillMode::Enabled: return MselCanKillMode::ENABLED;
    case msel::CanKillMode::Adr: return MselCanKillMode::ACCIDENT_DATA_RECORDER;
    }
    return MselCanKillMode::UNKNOWN;
}

std::optional<msel::CanKillMode> fromSchema(MselCanKillMode mode)
{
    switch (mode)
    {
    case MselCanKillMode::DISABLED: return msel::CanKillMode::Disabled;
    case MselCanKillMode::ENABLED: return msel::CanKillMode::Enabled;
    case MselCanKillMode::ACCIDENT_DATA_RECORDER: return msel::CanKillMode::Adr;
    case MselCanKillMode::UNKNOWN: return std::nullopt;
    }
    return std::nullopt;
}

MselSwitchState toSchema(const std::optional<msel::SwitchState>& state)
{
    if (!state)
    {
        return MselSwitchState::UNKNOWN;
    }

    switch (*state)
    {
    case msel::SwitchState::NormalCalNormalExternalSwitch:
        return MselSwitchState::NORMAL_CAL_NORMAL_EXTERNAL_SWITCH;
    case msel::SwitchState::NormalCalInternalExternalSwitch:
        return MselSwitchState::NORMAL_CAL_INTERNAL_EXTERNAL_SWITCH;
    case msel::SwitchState::LegacyCalNormalExternalSwitch:
        return MselSwitchState::LEGACY_CAL_NORMAL_EXTERNAL_SWITCH;
    case msel::SwitchState::LegacyCalInternalExternalSwitch:
        return MselSwitchState::LEGACY_CAL_INTERNAL_EXTERNAL_SWITCH;
    }
    return MselSwitchState::UNKNOWN;
}

msel::TransmitRate fromSchema(MselTransmitRate rate)
{
    switch (rate)
    {
    case MselTransmitRate::HZ10: return msel::TransmitRate::Hz10;
    case MselTransmitRate::HZ100: return msel::TransmitRate::Hz100;
    }
    return msel::TransmitRate::Hz10;
}

MselConfigResponse toSchema(msel::ConfigResponse response)
{
    switch (response)
    {
    case msel::ConfigResponse::Success: return MselConfigResponse::SUCCESS;
    case msel::ConfigResponse::IdMismatch: return MselConfigResponse::ID_MISMATCH;
    case msel::ConfigResponse::FrameCheckError: return MselConfigResponse::FRAME_CHECK_ERROR;
    case msel::ConfigResponse::InvalidId: return MselConfigResponse::INVALID_ID;
    }
    return MselConfigResponse::FRAME_CHECK_ERROR;
}

void fillConfig(const msel::Config& config, MselMasterRelayConfig::Builder out)
{
    out.setCanKill(toSchema(config.canKill));
    out.setCanKillRaw(config.canKillRaw);
    out.setBaud(toSchema(config.baud));
    out.setBaudRaw(config.baudRaw);
    out.setOutputDrive(toSchema(config.outputDrive));
    out.setOutputDriveRaw(config.outputDriveRaw);
    out.setShutdownDelayMs(static_cast<uint16_t>(config.shutdownDelay.count()));
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("msel_master_relay",
                             "MSEL Master Relay node: publishes isolator state and exposes its "
                             "settings as services");
    options.add_options()
        ("c,config", "YAML configuration file", cxxopts::value<std::string>())
        ("v,verbose", "Log every decoded frame")
        ("h,help", "Print usage");

    cxxopts::ParseResult parsed;
    try
    {
        parsed = options.parse(argc, argv);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("{}", error.what());
        return 2;
    }

    if (parsed.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    spdlog::set_level(parsed.count("verbose") ? spdlog::level::debug : spdlog::level::info);

    msel_node::NodeConfig config;
    if (parsed.count("config"))
    {
        if (!msel_node::load_node_config(parsed["config"].as<std::string>(), config))
        {
            return 2;
        }
    }
    else
    {
        SPDLOG_INFO("[node] no --config given; using defaults (base 0x{:X} on '{}')",
                    config.baseAddress, config.rxKey);
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps.
    pub_sub::NodeIdentity nodeIdentity("msel_master_relay");

    const std::string prefix = config.topicPrefix;
    pub_sub::ZenohPublisher<MselMasterRelayStatus> statusPublisher(prefix + "/status");
    pub_sub::ZenohPublisher<MselMasterRelayInfo> infoPublisher(prefix + "/info");
    pub_sub::ZenohPublisher<MselMasterRelaySwitchState> switchStatePublisher(prefix +
                                                                            "/switch_state");
    pub_sub::ZenohPublisher<MselMasterRelayConfig> configPublisher(prefix + "/config");
    pub_sub::ZenohPublisher<MselMasterRelayConfigResponse> configResponsePublisher(
        prefix + "/config_response");
    pub_sub::ZenohPublisher<CanFrame> txPublisher(config.txKey);

    // Zenoh delivers on its own receive threads, and a service call arrives on
    // another. Everything below touches the decoder or a publisher, and neither
    // is thread safe, so one lock covers the lot. There is nothing here slow
    // enough to be worth finer granularity: a 10Hz relay is 30 frames a second.
    std::mutex mutex;

    msel::Decoder decoder(msel::Addresses { .base = config.baseAddress });

    // Filling and publishing are one function because ZenohPublisher::put()
    // re-roots its builder: a second put() without refilling the fields would
    // publish a default-constructed message, so the periodic republish below
    // would quietly replace real telemetry with zero volts and an unknown
    // state rather than repeating the last reading.
    const auto publishStatus = [&](const msel::StatusFrame& frame) {
        auto& out = statusPublisher.fields();
        out.setStatus(frame.statusRaw);
        out.setState(toSchema(frame.status));
        out.setWarnings(frame.warnings);
        out.setOverTempWarn((frame.warnings & static_cast<uint8_t>(msel::Warning::OverTemperature)) != 0u);
        out.setOverCurrentWarn((frame.warnings & static_cast<uint8_t>(msel::Warning::OverCurrent)) != 0u);
        out.setLowVoltageWarn((frame.warnings & static_cast<uint8_t>(msel::Warning::LowVoltage)) != 0u);
        out.setHighVoltageWarn((frame.warnings & static_cast<uint8_t>(msel::Warning::HighVoltage)) != 0u);
        out.setOverTempKill((frame.warnings & static_cast<uint8_t>(msel::Warning::OverTemperatureKill)) != 0u);
        out.setDriverKill((frame.warnings & static_cast<uint8_t>(msel::Warning::DriverSwitchKill)) != 0u);
        out.setExternalKill((frame.warnings & static_cast<uint8_t>(msel::Warning::ExternalSwitchKill)) != 0u);
        out.setCanKill((frame.warnings & static_cast<uint8_t>(msel::Warning::CanTriggerKill)) != 0u);
        out.setTemperatureInternal(static_cast<float>(frame.temperatureInternal));
        out.setLoadCurrent(static_cast<float>(frame.loadCurrent));
        out.setVoltageOut(static_cast<float>(frame.voltageOut));
        statusPublisher.put();
    };

    decoder.onStatus(publishStatus);

    decoder.onInfo([&](const msel::InfoFrame& frame) {
        auto& out = infoPublisher.fields();
        out.setShutdownCause(frame.shutdownCauseRaw);
        out.setShutdownCause2(frame.shutdownCause2Raw);
        out.setShutdownCauseState(toSchema(frame.shutdownCause));
        out.setShutdownCause2State(toSchema(frame.shutdownCause2));
        out.setTimeSinceShutdown(static_cast<float>(frame.timeSinceShutdown.count()) / 1000.0f);
        out.setConfigShutdownDelay(static_cast<float>(frame.config.shutdownDelay.count()) / 1000.0f);
        out.setConfigCanKill(frame.config.canKillRaw);
        out.setConfigCanBaud(frame.config.baudRaw);
        out.setConfigOutputDrive(frame.config.outputDriveRaw);
        out.setSerialNo(frame.serialNo);
        out.setVoltageIn(static_cast<float>(frame.voltageIn));
        infoPublisher.put();

        fillConfig(frame.config, configPublisher.fields());
        configPublisher.put();
    });

    decoder.onSwitchState([&](const msel::SwitchStateFrame& frame) {
        auto& out = switchStatePublisher.fields();
        out.setSwitchState(toSchema(frame.switchState));
        out.setSwitchStateRaw(frame.switchStateRaw);
        switchStatePublisher.put();
    });

    decoder.onConfigResponse([&](msel::ConfigResponse response) {
        // Loud on purpose. This is the relay's only acknowledgement that a
        // settings change landed, and it arrives seconds after the service call
        // that caused it has already returned.
        if (response == msel::ConfigResponse::Success)
        {
            SPDLOG_INFO("[relay] configuration accepted");
        }
        else
        {
            SPDLOG_WARN("[relay] configuration rejected: {}", msel::to_string(response));
        }

        auto& out = configResponsePublisher.fields();
        out.setResponse(toSchema(response));
        out.setResponseRaw(static_cast<uint8_t>(response));
        configResponsePublisher.put();
    });

    // --- transmit ----------------------------------------------------------
    //
    // One place where a frame goes onto the bus, so that every command is
    // logged and rendered the same way, and the response every service returns
    // says exactly what was sent.
    const auto sendCommand = [&](const msel::Result<helpers::CanFrame>& built,
                                 const char* what,
                                 MselCommandResponse::Builder& response) {
        if (!built)
        {
            SPDLOG_WARN("[node] refusing to {}: {}", what, built.error().message);
            response.setOk(false);
            response.setError(built.error().message);
            response.setFrameSent("");
            return;
        }

        const auto& frame = *built;
        const std::string hex = msel::toHex(frame.data_span());
        const bool needsPowerCycle = msel::effectOf(frame) == msel::CommandEffect::RequiresPowerCycle;

        auto& out = txPublisher.fields();
        out.setId(frame.id);
        out.setLen(frame.len);
        auto data = out.initData(frame.len);
        for (unsigned i = 0u; i < frame.len; ++i)
        {
            data.set(i, frame.data[i]);
        }
        out.setExtended(false);
        out.setRtr(false);
        out.setFd(false);
        out.setChannel("");
        txPublisher.put();

        SPDLOG_INFO("[node] {}: sent 0x{:X} [{}]{}", what, frame.id, hex,
                    needsPowerCycle ? " (needs a power cycle)" : "");

        // Only the five configuration commands need the switch held. The kill
        // trigger is not one of them -- it is acted on the moment it arrives,
        // which is the entire point of it -- so saying otherwise here would
        // contradict the flag this same function puts in the response.
        const bool isConfigCommand = frame.id == msel::kConfigCommandId;
        if (isConfigCommand)
        {
            SPDLOG_INFO("[node] the relay ignores this unless the external kill switch is held "
                        "down while it arrives");
        }

        response.setOk(true);
        response.setError("");
        response.setFrameSent(hex);
        response.setRequiresPowerCycle(needsPowerCycle);
        response.setHoldExternalKillSwitch(isConfigCommand &&
                                           msel::requiresHeldExternalKillSwitch());
    };

    // --- receive -----------------------------------------------------------
    pub_sub::ZenohTypedSubscriber<CanFrame> canSubscriber(
        config.rxKey, [&](CanFrame::Reader reader) {
            helpers::CanFrame frame;
            frame.id = reader.getId();
            frame.isExtended = reader.getExtended();
            frame.isRTR = reader.getRtr();
            frame.isFD = reader.getFd();
            frame.isError = reader.getError();
            frame.timestampUs = reader.getTimestampUs();

            const auto payload = reader.getData();
            // The real length, not the padded buffer: a frame shorter than the
            // message it claims to be must be rejected, not decoded as though
            // the padding were readings.
            const size_t length =
                std::min<size_t>(frame.data.size(), std::min<size_t>(reader.getLen(), payload.size()));
            frame.len = static_cast<uint8_t>(length);
            for (size_t i = 0u; i < length; ++i)
            {
                frame.data[i] = payload[static_cast<unsigned>(i)];
            }

            const std::lock_guard<std::mutex> lock(mutex);
            const auto accepted = decoder.onFrame(frame);
            if (accepted != msel::Decoder::Accepted::No)
            {
                SPDLOG_DEBUG("[relay] decoded 0x{:X} [{}]", frame.id,
                             msel::toHex(frame.data_span()));
            }
        });

    // --- services ----------------------------------------------------------

    pub_sub::ZenohService<MselGetSettingsRequest, MselGetSettingsResponse> getSettings(
        prefix + "/get_settings",
        [&](const MselGetSettingsRequest::Reader&, MselGetSettingsResponse::Builder& response) {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto& snapshot = decoder.snapshot();

            response.setOk(true);
            response.setError("");
            response.setBaseAddress(static_cast<uint16_t>(decoder.addresses().base));
            response.setKillAddress(static_cast<uint16_t>(config.killAddress));
            response.setCanKillAllowed(config.allowCanKill);

            if (!snapshot.info)
            {
                // Not an error: it is the honest answer before the relay's info
                // message has been seen, and it is the difference between "the
                // delay is zero" and "we have not heard from the relay".
                response.setValid(false);
                response.setSerialNo(0u);
                fillConfig(msel::Config {}, response.initConfig());
                return;
            }

            response.setValid(true);
            response.setSerialNo(snapshot.info->serialNo);
            fillConfig(snapshot.info->config, response.initConfig());
        });

    pub_sub::ZenohService<MselSetBaseAddressRequest, MselCommandResponse> setBaseAddress(
        prefix + "/set_base_address",
        [&](const MselSetBaseAddressRequest::Reader& request,
            MselCommandResponse::Builder& response) {
            const auto requested = static_cast<uint32_t>(request.getBaseAddress());
            const std::lock_guard<std::mutex> lock(mutex);
            sendCommand(msel::makeSetBaseAddressFrame(requested), "set the base address", response);

            if (response.getOk())
            {
                // The relay moves on acknowledge, so the decoder has to move
                // with it or telemetry stops at exactly the moment the change
                // succeeds. Nothing else on the bus is told, which is why this
                // is a warning rather than an info line.
                decoder.setAddresses(msel::Addresses { .base = requested });
                SPDLOG_WARN("[node] now decoding at base 0x{:X}. Every other consumer of this "
                            "relay -- loggers, dashes, DBC imports -- is still looking at the old "
                            "identifiers",
                            requested);
            }
        });

    pub_sub::ZenohService<MselSetTransmitRateRequest, MselCommandResponse> setTransmitRate(
        prefix + "/set_transmit_rate",
        [&](const MselSetTransmitRateRequest::Reader& request,
            MselCommandResponse::Builder& response) {
            const std::lock_guard<std::mutex> lock(mutex);
            sendCommand(msel::makeSetTransmitRateFrame(fromSchema(request.getRate())),
                        "set the transmit rate", response);
        });

    pub_sub::ZenohService<MselSetBaudAndShutdownDelayRequest, MselCommandResponse>
        setBaudAndShutdownDelay(
            prefix + "/set_baud_and_shutdown_delay",
            [&](const MselSetBaudAndShutdownDelayRequest::Reader& request,
                MselCommandResponse::Builder& response) {
                const auto baud = fromSchema(request.getBaud());
                if (!baud)
                {
                    response.setOk(false);
                    response.setError("baud must be one of rate1Mbps, rate500Kbps or rate250Kbps");
                    return;
                }

                const std::lock_guard<std::mutex> lock(mutex);
                sendCommand(msel::makeSetBaudAndShutdownDelayFrame(
                                *baud, std::chrono::milliseconds { request.getShutdownDelayMs() }),
                            "set the baud rate and shutdown delay", response);

                if (response.getOk())
                {
                    SPDLOG_WARN("[node] the new bus rate takes effect when the relay is power "
                                "cycled. If the rest of the bus is not moved to match, the relay "
                                "will not be heard from again");
                }
            });

    pub_sub::ZenohService<MselSetOutputDriveRequest, MselCommandResponse> setOutputDrive(
        prefix + "/set_output_drive",
        [&](const MselSetOutputDriveRequest::Reader& request,
            MselCommandResponse::Builder& response) {
            const auto drive = fromSchema(request.getDrive());
            if (!drive)
            {
                response.setOk(false);
                response.setError("drive must be one of the six documented output drive modes");
                return;
            }

            const std::lock_guard<std::mutex> lock(mutex);
            sendCommand(msel::makeSetOutputDriveFrame(*drive), "set the output drive", response);
        });

    pub_sub::ZenohService<MselSetCanShutdownRequest, MselCommandResponse> setCanShutdown(
        prefix + "/set_can_shutdown",
        [&](const MselSetCanShutdownRequest::Reader& request,
            MselCommandResponse::Builder& response) {
            const auto mode = fromSchema(request.getMode());
            if (!mode)
            {
                response.setOk(false);
                response.setError(
                    "mode must be one of disabled, enabled or accidentDataRecorder");
                return;
            }

            const std::lock_guard<std::mutex> lock(mutex);
            sendCommand(
                msel::makeSetCanShutdownFrame(*mode, static_cast<uint32_t>(request.getKillAddress())),
                "configure remote shutdown", response);
        });

    pub_sub::ZenohService<MselCanKillRequest, MselCommandResponse> canKill(
        prefix + "/can_kill",
        [&](const MselCanKillRequest::Reader& request, MselCommandResponse::Builder& response) {
            // Two gates. Neither a config file left permissive on a bench nor a
            // stray service call should be able to stop a moving vehicle by
            // itself.
            if (!config.allowCanKill)
            {
                const std::string error =
                    "remote shutdown is not permitted: start this node with allow_can_kill: true "
                    "in its config";
                SPDLOG_WARN("[node] refusing remote shutdown: not permitted by config");
                response.setOk(false);
                response.setError(error);
                return;
            }

            if (request.getConfirm().cStr() != std::string(msel_node::kCanKillConfirmToken))
            {
                const std::string error =
                    fmt::format("remote shutdown needs confirm to be exactly '{}'",
                                msel_node::kCanKillConfirmToken);
                SPDLOG_WARN("[node] refusing remote shutdown: confirmation token absent or wrong");
                response.setOk(false);
                response.setError(error);
                return;
            }

            SPDLOG_WARN("[node] transmitting a remote shutdown on 0x{:X}. This isolates the "
                        "battery and stops the engine",
                        config.killAddress);

            const std::lock_guard<std::mutex> lock(mutex);
            sendCommand(msel::makeCanKillTriggerFrame(config.killAddress),
                        "trigger a remote shutdown", response);
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SPDLOG_INFO("[node] decoding a Master Relay at base 0x{:X} ({:#X}/{:#X}/{:#X}) from '{}'",
                config.baseAddress, config.baseAddress, config.baseAddress + 1u,
                config.baseAddress + 3u, config.rxKey);
    SPDLOG_INFO("[node] publishing under '{}', commands go out on '{}'", prefix, config.txKey);
    if (config.allowCanKill)
    {
        SPDLOG_WARN("[node] remote shutdown is PERMITTED on 0x{:X}", config.killAddress);
    }

    auto nextStatus = std::chrono::steady_clock::now();
    while (gRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (config.statusIntervalMs == 0u)
        {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus)
        {
            nextStatus = now + std::chrono::milliseconds(config.statusIntervalMs);

            const std::lock_guard<std::mutex> lock(mutex);
            if (const auto& last = decoder.snapshot().status)
            {
                publishStatus(*last);
            }
        }
    }

    SPDLOG_INFO("[node] shutting down");
    return 0;
}
