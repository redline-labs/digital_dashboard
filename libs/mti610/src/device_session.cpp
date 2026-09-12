// SPDX-License-Identifier: GPL-3.0-or-later

#include "mti610/device_session.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>

namespace mti610
{
namespace
{

using Clock = std::chrono::steady_clock;

// How long one pump() waits for bytes. Short enough that the reply deadline is
// honoured to within a read, long enough not to spin.
constexpr unsigned kPumpSliceMs = 20;

std::string warningText(std::span<const std::uint8_t> payload)
{
    // MID 0x43 carries a uint32 result value and a 128-byte string. The string
    // is taken up to its first NUL; a device that sent an unterminated one
    // would otherwise leak whatever followed into a log line.
    if (payload.size() <= 4)
    {
        return {};
    }

    const std::span<const std::uint8_t> text = payload.subspan(4);
    std::size_t end = 0;
    while (end < text.size() && text[end] != 0)
    {
        ++end;
    }

    return std::string(reinterpret_cast<const char*>(text.data()), end);
}

} // namespace

std::string DeviceInfo::firmwareVersion() const
{
    return std::to_string(firmwareMajor) + "." + std::to_string(firmwareMinor) + "." +
           std::to_string(firmwareRevision) + "." + std::to_string(firmwareBuild);
}

bool DeviceInfo::looksLikeMti610() const
{
    return productCode.rfind("MTi-610", 0) == 0;
}

DeviceSession::DeviceSession(ByteStream& stream, xbus::Framer& framer, SessionOptions options) :
    mStream(stream),
    mFramer(framer),
    mOptions(options)
{
}

bool DeviceSession::pump(unsigned timeoutMs)
{
    std::array<std::uint8_t, 512> buffer {};
    const ssize_t n = mStream.recvSome(buffer, timeoutMs);

    if (n < 0)
    {
        return false;
    }

    if (n > 0)
    {
        mFramer.push(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(n)));
    }

    return true;
}

Result<xbus::MessageView> DeviceSession::exchangeOnce(std::span<const std::uint8_t> command,
                                                      xbus::MessageId expectedAck, bool& sawWakeUp)
{
    sawWakeUp = false;

    if (!mStream.isOpen())
    {
        return not_connected("the port is not open");
    }

    if (!mStream.sendAll(command))
    {
        return io_error("cannot write to the port");
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(mOptions.replyTimeoutMs);

    while (Clock::now() < deadline)
    {
        // Drain whatever is already framed before reading more: the reply may
        // well be sitting behind the data messages that arrived with it.
        while (const std::optional<xbus::MessageView> message = mFramer.next())
        {
            if (message->id == static_cast<std::uint8_t>(expectedAck))
            {
                return *message;
            }

            if (message->is(xbus::MessageId::ErrorReport))
            {
                const xbus::Result<xbus::ErrorReply> reply =
                    xbus::ErrorReply::parse(message->data);

                if (!reply)
                {
                    return protocol_error("the device reported an error with no code");
                }

                return refused(xbus::to_string(reply->deviceError()));
            }

            if (message->is(xbus::MessageId::WarningReport))
            {
                if (mOnWarning && message->data.size() >= 4)
                {
                    mOnWarning(xbus::read_u32(message->data, 0), warningText(message->data));
                }
                continue;
            }

            if (message->is(xbus::MessageId::WakeUp))
            {
                // The device reset. Whatever state this exchange assumed is
                // gone, so answering and starting over is the only correct
                // response -- and it has to happen inside 500 ms.
                SPDLOG_WARN("mti610: the device reset mid-exchange");
                sendWakeUpAck();
                sawWakeUp = true;
                return protocol_error("the device reset");
            }

            if (message->is(xbus::MessageId::MtData2))
            {
                // Still in Measurement, or the queue has not drained yet.
                if (mOnData)
                {
                    mOnData(*message);
                }
                continue;
            }

            // Something else entirely -- an acknowledgement for a command this
            // exchange did not send, which happens when a previous exchange
            // timed out and its reply arrived late. Skipped rather than
            // treated as a failure, because failing here would make one late
            // reply poison every exchange after it.
            SPDLOG_DEBUG("mti610: skipping unexpected message id 0x{:02X} while waiting for {}",
                         message->id, xbus::message_name(expectedAck));
        }

        if (!pump(kPumpSliceMs))
        {
            return io_error("the port closed while waiting for a reply");
        }
    }

    return timeout(std::string("no ") + xbus::message_name(expectedAck) + " from the device");
}

Result<xbus::MessageView> DeviceSession::exchange(std::span<const std::uint8_t> command,
                                                  xbus::MessageId expectedAck)
{
    Result<xbus::MessageView> result = not_connected("not attempted");

    for (unsigned attempt = 0; attempt <= mOptions.retries; ++attempt)
    {
        bool sawWakeUp = false;
        result = exchangeOnce(command, expectedAck, sawWakeUp);

        if (result.has_value())
        {
            return result;
        }

        // A refusal is the device's considered answer. Sending the same bytes
        // again will get the same answer, so retrying only delays the report.
        if (result.error().kind == Error::Kind::Refused)
        {
            return result;
        }

        if (result.error().kind == Error::Kind::Io ||
            result.error().kind == Error::Kind::NotConnected)
        {
            return result;
        }

        if (sawWakeUp)
        {
            // The device is now in Config, having just been told to be. Retry
            // the command rather than the whole sequence: the caller's own
            // configure() re-reads afterwards anyway.
            continue;
        }

        if (attempt < mOptions.retries)
        {
            SPDLOG_DEBUG("mti610: retrying, no {} (attempt {} of {})",
                         xbus::message_name(expectedAck), attempt + 1, mOptions.retries + 1);
        }
    }

    return result;
}

bool DeviceSession::sendWakeUpAck()
{
    return mStream.sendAll(xbus::kWakeUpAck);
}

Result<void> DeviceSession::goToConfig()
{
    const Result<xbus::MessageView> reply =
        exchange(xbus::kGoToConfig, xbus::MessageId::GoToConfigAck);

    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    return {};
}

Result<void> DeviceSession::goToMeasurement()
{
    const Result<xbus::MessageView> reply =
        exchange(xbus::kGoToMeasurement, xbus::MessageId::GoToMeasurementAck);

    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    return {};
}

Result<DeviceInfo> DeviceSession::identify()
{
    DeviceInfo info;

    const Result<xbus::MessageView> id =
        exchange(xbus::kReqDeviceId, xbus::MessageId::DeviceId);
    if (!id)
    {
        return std::unexpected(id.error());
    }

    const xbus::Result<xbus::DeviceIdReply> deviceId = xbus::DeviceIdReply::parse(id->data);
    if (!deviceId)
    {
        return protocol_error("the device id reply was " + std::to_string(id->data.size()) +
                              " bytes, which is neither the four-byte nor the eight-byte form");
    }
    info.deviceId = deviceId->deviceId;

    // The remaining three are informational. A device that declines one is
    // still usable, so a failure here is logged and the field left at its
    // default rather than failing the whole identification -- which would make
    // an unsupported informational message look like a dead device.
    if (const Result<xbus::MessageView> code =
            exchange(xbus::kReqProductCode, xbus::MessageId::ProductCode))
    {
        info.productCode = std::string(xbus::parse_product_code(code->data));
    }
    else
    {
        SPDLOG_WARN("mti610: no product code: {}", to_string(code.error()));
    }

    if (const Result<xbus::MessageView> fw =
            exchange(xbus::kReqFirmwareRevision, xbus::MessageId::FirmwareRevision))
    {
        if (const xbus::Result<xbus::FirmwareRevision> parsed =
                xbus::FirmwareRevision::parse(fw->data))
        {
            info.firmwareMajor = parsed->major;
            info.firmwareMinor = parsed->minor;
            info.firmwareRevision = parsed->revision;
            info.firmwareBuild = parsed->buildNumber;
        }
    }

    if (const Result<xbus::MessageView> hw =
            exchange(xbus::kReqHardwareVersion, xbus::MessageId::HardwareVersion))
    {
        if (const xbus::Result<xbus::HardwareVersion> parsed =
                xbus::HardwareVersion::parse(hw->data))
        {
            info.hardwareMajor = parsed->major;
            info.hardwareMinor = parsed->minor;
        }
    }

    return info;
}

Result<std::vector<OutputEntry>> DeviceSession::readOutputConfig()
{
    const Result<xbus::MessageView> reply =
        exchange(xbus::kReqOutputConfiguration, xbus::MessageId::OutputConfigurationAck);

    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    const xbus::Result<std::size_t> count = xbus::output_entry_count(reply->data);
    if (!count)
    {
        return protocol_error("the output configuration reply was " +
                              std::to_string(reply->data.size()) +
                              " bytes, which is not a whole number of four-byte entries");
    }

    std::vector<OutputEntry> entries;
    entries.reserve(*count);

    for (std::size_t i = 0; i < *count; ++i)
    {
        const xbus::Result<OutputEntry> entry = xbus::parse_output_entry(reply->data, i);
        if (!entry)
        {
            return protocol_error("the output configuration reply is truncated at entry " +
                                  std::to_string(i));
        }
        entries.push_back(*entry);
    }

    return entries;
}

Result<std::vector<OutputEntry>> DeviceSession::writeOutputConfig(
    const std::vector<OutputEntry>& entries)
{
    std::array<std::uint8_t, xbus::kMaxOutputEntries * xbus::kOutputEntrySize> payload {};
    const xbus::Result<std::size_t> payloadSize = xbus::encode_output_config(entries, payload);

    if (!payloadSize)
    {
        return invalid_argument("the output list does not fit in one message: " +
                                std::to_string(entries.size()) + " entries, limit " +
                                std::to_string(xbus::kMaxOutputEntries));
    }

    std::array<std::uint8_t, xbus::kMaxOutputEntries * xbus::kOutputEntrySize +
                                 xbus::kHeaderSize + xbus::kChecksumSize>
        message {};

    const xbus::Result<std::size_t> messageSize = xbus::encode_message(
        xbus::MessageId::OutputConfiguration,
        std::span<const std::uint8_t>(payload.data(), *payloadSize), message);

    if (!messageSize)
    {
        return invalid_argument("the output configuration message does not fit");
    }

    const Result<xbus::MessageView> reply =
        exchange(std::span<const std::uint8_t>(message.data(), *messageSize),
                 xbus::MessageId::OutputConfigurationAck);

    if (!reply)
    {
        return std::unexpected(reply.error());
    }

    // The acknowledgement carries the list the device will ACTUALLY send,
    // which is not necessarily the one we asked for: it clamps a rate it
    // cannot sustain rather than refusing the message. Parsing the echo rather
    // than assuming the write took is what makes the node's status message
    // true.
    const xbus::Result<std::size_t> count = xbus::output_entry_count(reply->data);
    if (!count)
    {
        return protocol_error("the device echoed a malformed output configuration");
    }

    std::vector<OutputEntry> effective;
    effective.reserve(*count);

    for (std::size_t i = 0; i < *count; ++i)
    {
        const xbus::Result<OutputEntry> entry = xbus::parse_output_entry(reply->data, i);
        if (!entry)
        {
            return protocol_error("the device's echoed configuration is truncated");
        }
        effective.push_back(*entry);
    }

    return effective;
}

Result<ConfigureResult> DeviceSession::configure(const std::vector<OutputEntry>& desired)
{
    const Result<std::vector<OutputEntry>> actual = readOutputConfig();
    if (!actual)
    {
        return std::unexpected(actual.error());
    }

    ConfigureResult result;
    result.changes = diff(*actual, desired);
    result.effective = *actual;

    if (is_satisfied(result.changes, mOptions.policy))
    {
        return result;
    }

    for (const Change& change : result.changes)
    {
        SPDLOG_INFO("mti610: {}", to_string(change));
    }

    if (mOptions.mode == ConfigMode::ReportOnly)
    {
        // Deliberately not an error. report_only exists for a device somebody
        // else owns, and a node that failed to start because of it would be
        // useless for the case the mode was added for.
        SPDLOG_INFO("mti610: report_only, leaving the configuration alone");
        return result;
    }

    const std::vector<OutputEntry> planned = plan_writes(*actual, desired, mOptions.policy);

    const Result<std::vector<OutputEntry>> effective = writeOutputConfig(planned);
    if (!effective)
    {
        return std::unexpected(effective.error());
    }

    result.wrote = true;
    result.effective = *effective;

    // Report what the device settled on, where that is not what was asked. It
    // clamps rather than refusing, so a rate the link cannot carry becomes a
    // lower rate and no error -- which looks like a device running slow.
    for (const OutputEntry& want : planned)
    {
        for (const OutputEntry& got : result.effective)
        {
            if (got.rawId == want.rawId && got.frequencyHz != want.frequencyHz)
            {
                SPDLOG_WARN("mti610: asked 0x{:04X} for {} Hz, the device settled on {} Hz",
                            want.rawId, want.frequencyHz, got.frequencyHz);
            }
        }
    }

    return result;
}

} // namespace mti610
