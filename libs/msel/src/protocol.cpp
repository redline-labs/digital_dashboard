// SPDX-License-Identifier: GPL-3.0-or-later

#include "msel/protocol.h"

#include <array>
#include <spdlog/fmt/fmt.h>

namespace msel
{

namespace
{

// The magic word each configuration command carries in bytes 0-1 and again in
// bytes 6-7. There is no command byte and no length field: the magic is the
// only thing that says which of the five this is, which is why a frame that
// gets one wrong comes back as a frame check error rather than being ignored.
constexpr uint16_t kMagicBaseAddress = 0x0789u;
constexpr uint16_t kMagicTransmitRate = 0x0DEFu;
constexpr uint16_t kMagicBaudAndDelay = 0x0456u;
constexpr uint16_t kMagicOutputDrive = 0x0ABCu;
constexpr uint16_t kMagicCanShutdown = 0x0123u;

constexpr size_t kCommandLength = 8u;

helpers::CanFrame makeFrame(uint32_t id, const std::array<uint8_t, kCommandLength>& bytes)
{
    helpers::CanFrame frame;
    frame.id = id;
    frame.len = static_cast<uint8_t>(kCommandLength);
    for (size_t i = 0u; i < kCommandLength; ++i)
    {
        frame.data[i] = bytes[i];
    }
    return frame;
}

// Two payload shapes exist, and they are not interchangeable.
//
// The byte form carries two independent 8-bit values, each written twice into
// adjacent bytes: [magic][a][a][b][b][magic]. The word form carries one 16-bit
// value written twice as a big-endian pair: [magic][hi][lo][hi][lo][magic].
// Confusing the two produces a frame the device answers with 0x22, so they are
// spelled out separately rather than folded into one helper with a flag.
std::array<uint8_t, kCommandLength> byteCommand(uint16_t magic, uint8_t a, uint8_t b)
{
    const auto hi = static_cast<uint8_t>(magic >> 8);
    const auto lo = static_cast<uint8_t>(magic & 0xFFu);
    return { hi, lo, a, a, b, b, hi, lo };
}

std::array<uint8_t, kCommandLength> wordCommand(uint16_t magic, uint16_t value)
{
    const auto hi = static_cast<uint8_t>(magic >> 8);
    const auto lo = static_cast<uint8_t>(magic & 0xFFu);
    const auto vhi = static_cast<uint8_t>(value >> 8);
    const auto vlo = static_cast<uint8_t>(value & 0xFFu);
    return { hi, lo, vhi, vlo, vhi, vlo, hi, lo };
}

uint16_t magicOf(const helpers::CanFrame& frame)
{
    if (frame.len < 2u)
    {
        return 0u;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(frame.data[0]) << 8) | frame.data[1]);
}

} // namespace

const char* to_string(Status value)
{
    switch (value)
    {
    case Status::Normal: return "normal";
    case Status::OverTemperatureWarning: return "over temperature warning";
    case Status::OverCurrentWarning: return "over current warning";
    case Status::LowVoltageWarning: return "low voltage warning";
    case Status::HighVoltageWarning: return "high voltage warning";
    case Status::OverTemperatureKill: return "over temperature kill";
    case Status::DriverSwitchKill: return "driver switch kill";
    case Status::ExternalSwitchKill: return "external switch kill";
    case Status::CanTriggerKill: return "CAN trigger kill";
    case Status::PowerOnReset: return "power on reset";
    }
    return "unknown";
}

const char* to_string(Warning value)
{
    switch (value)
    {
    case Warning::OverTemperature: return "over temperature";
    case Warning::OverCurrent: return "over current";
    case Warning::LowVoltage: return "low voltage";
    case Warning::HighVoltage: return "high voltage";
    case Warning::OverTemperatureKill: return "over temperature kill";
    case Warning::DriverSwitchKill: return "driver switch kill";
    case Warning::ExternalSwitchKill: return "external switch kill";
    case Warning::CanTriggerKill: return "CAN trigger kill";
    }
    return "unknown";
}

const char* to_string(CanBaud value)
{
    switch (value)
    {
    case CanBaud::Rate1M: return "1 Mbps";
    case CanBaud::Rate500k: return "500 kbps";
    case CanBaud::Rate250k: return "250 kbps";
    }
    return "unknown";
}

const char* to_string(OutputDrive value)
{
    switch (value)
    {
    case OutputDrive::ActiveHighHalfBridge: return "active high, half bridge";
    case OutputDrive::ActiveHighHighSide: return "active high, high side";
    case OutputDrive::ActiveHighLowSide: return "active high, low side";
    case OutputDrive::ActiveLowHalfBridge: return "active low, half bridge";
    case OutputDrive::ActiveLowHighSide: return "active low, high side";
    case OutputDrive::ActiveLowLowSide: return "active low, low side";
    }
    return "unknown";
}

const char* to_string(CanKillMode value)
{
    switch (value)
    {
    case CanKillMode::Disabled: return "disabled";
    case CanKillMode::Enabled: return "enabled";
    case CanKillMode::Adr: return "accident data recorder";
    }
    return "unknown";
}

const char* to_string(SwitchState value)
{
    switch (value)
    {
    case SwitchState::NormalCalNormalExternalSwitch:
        return "normal current calibration, normal external switch";
    case SwitchState::NormalCalInternalExternalSwitch:
        return "normal current calibration, internally acting external switch";
    case SwitchState::LegacyCalNormalExternalSwitch:
        return "legacy current calibration, normal external switch";
    case SwitchState::LegacyCalInternalExternalSwitch:
        return "legacy current calibration, internally acting external switch";
    }
    return "unknown";
}

const char* to_string(TransmitRate value)
{
    switch (value)
    {
    case TransmitRate::Hz10: return "10 Hz";
    case TransmitRate::Hz100: return "100 Hz";
    }
    return "unknown";
}

const char* to_string(ConfigResponse value)
{
    switch (value)
    {
    case ConfigResponse::Success: return "success";
    case ConfigResponse::IdMismatch: return "identifiers do not match";
    case ConfigResponse::FrameCheckError: return "frame check error";
    case ConfigResponse::InvalidId: return "invalid identifier";
    }
    return "unknown";
}

uint32_t toBitsPerSecond(CanBaud value)
{
    switch (value)
    {
    case CanBaud::Rate1M: return 1000000u;
    case CanBaud::Rate500k: return 500000u;
    case CanBaud::Rate250k: return 250000u;
    }
    return 0u;
}

std::optional<Status> statusFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 1: return Status::Normal;
    case 2: return Status::OverTemperatureWarning;
    case 3: return Status::OverCurrentWarning;
    case 4: return Status::LowVoltageWarning;
    case 5: return Status::HighVoltageWarning;
    case 6: return Status::OverTemperatureKill;
    case 7: return Status::DriverSwitchKill;
    case 8: return Status::ExternalSwitchKill;
    case 9: return Status::CanTriggerKill;
    case 10: return Status::PowerOnReset;
    default: return std::nullopt;
    }
}

std::optional<CanBaud> canBaudFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 0: return CanBaud::Rate1M;
    case 1: return CanBaud::Rate500k;
    case 2: return CanBaud::Rate250k;
    default: return std::nullopt;
    }
}

std::optional<OutputDrive> outputDriveFromRaw(uint8_t raw)
{
    // 3 and 7 are deliberately absent: the manual assigns no meaning to them,
    // and mapping them onto a neighbour would invent a configuration the device
    // is not in.
    switch (raw)
    {
    case 0: return OutputDrive::ActiveHighHalfBridge;
    case 1: return OutputDrive::ActiveHighHighSide;
    case 2: return OutputDrive::ActiveHighLowSide;
    case 4: return OutputDrive::ActiveLowHalfBridge;
    case 5: return OutputDrive::ActiveLowHighSide;
    case 6: return OutputDrive::ActiveLowLowSide;
    default: return std::nullopt;
    }
}

std::optional<CanKillMode> canKillModeFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 0: return CanKillMode::Disabled;
    case 1: return CanKillMode::Enabled;
    case 2: return CanKillMode::Adr;
    default: return std::nullopt;
    }
}

std::optional<SwitchState> switchStateFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 1: return SwitchState::NormalCalNormalExternalSwitch;
    case 2: return SwitchState::NormalCalInternalExternalSwitch;
    case 3: return SwitchState::LegacyCalNormalExternalSwitch;
    case 4: return SwitchState::LegacyCalInternalExternalSwitch;
    default: return std::nullopt;
    }
}

std::optional<TransmitRate> transmitRateFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 0x01: return TransmitRate::Hz10;
    case 0x0A: return TransmitRate::Hz100;
    default: return std::nullopt;
    }
}

std::string describeWarnings(uint8_t mask)
{
    constexpr std::array<Warning, 8> kAll = {
        Warning::OverTemperature,      Warning::OverCurrent,
        Warning::LowVoltage,           Warning::HighVoltage,
        Warning::OverTemperatureKill,  Warning::DriverSwitchKill,
        Warning::ExternalSwitchKill,   Warning::CanTriggerKill,
    };

    std::string out;
    for (const Warning warning : kAll)
    {
        if ((mask & static_cast<uint8_t>(warning)) != 0u)
        {
            if (!out.empty())
            {
                out += "|";
            }
            out += to_string(warning);
        }
    }
    return out;
}

Result<void> validateBaseAddress(uint32_t base)
{
    // base+3 is the highest message, so that is what has to fit -- checking
    // only `base` would accept 0x7FF and then place the switch-state message at
    // 0x802, which is not a standard identifier at all.
    if (base > kMaxStandardId || (base + 3u) > kMaxStandardId)
    {
        return invalid_argument(fmt::format(
            "base address 0x{:X} does not fit: base+3 must be no more than 0x{:X}", base,
            kMaxStandardId));
    }

    if (base <= kConfigCommandId && kConfigCommandId <= (base + 3u))
    {
        return invalid_argument(fmt::format(
            "base address 0x{:X} spans the configuration identifier 0x{:X}, which would leave the "
            "relay unable to be reconfigured",
            base, kConfigCommandId));
    }

    return {};
}

Config decodeConfigWord(uint16_t word)
{
    const auto byte4 = static_cast<uint8_t>(word >> 8);
    const auto byte5 = static_cast<uint8_t>(word & 0xFFu);

    Config config;
    config.canKillRaw = static_cast<uint8_t>((byte4 >> 6) & 0x03u);
    config.baudRaw = static_cast<uint8_t>((byte4 >> 4) & 0x03u);
    config.outputDriveRaw = static_cast<uint8_t>(byte4 & 0x0Fu);
    config.canKill = canKillModeFromRaw(config.canKillRaw);
    config.baud = canBaudFromRaw(config.baudRaw);
    config.outputDrive = outputDriveFromRaw(config.outputDriveRaw);
    config.shutdownDelay = std::chrono::milliseconds { static_cast<int64_t>(byte5) * 100 };
    return config;
}

uint16_t encodeConfigWord(const Config& config)
{
    // Encodes from the raw fields, not the optionals: this has to be able to
    // reproduce whatever the device reported, including a value the manual does
    // not document, or a round trip through it would quietly normalise state
    // that the relay is actually in.
    const auto byte4 = static_cast<uint8_t>(((config.canKillRaw & 0x03u) << 6) |
                                            ((config.baudRaw & 0x03u) << 4) |
                                            (config.outputDriveRaw & 0x0Fu));
    const auto tenths = config.shutdownDelay.count() / 100;
    const auto byte5 = static_cast<uint8_t>(tenths < 0 ? 0 : (tenths > 255 ? 255 : tenths));
    return static_cast<uint16_t>((static_cast<uint16_t>(byte4) << 8) | byte5);
}

Result<helpers::CanFrame> makeSetBaseAddressFrame(uint32_t newBase)
{
    if (auto valid = validateBaseAddress(newBase); !valid)
    {
        return std::unexpected(valid.error());
    }

    return makeFrame(kConfigCommandId,
                     wordCommand(kMagicBaseAddress, static_cast<uint16_t>(newBase)));
}

Result<helpers::CanFrame> makeSetTransmitRateFrame(TransmitRate rate)
{
    return makeFrame(kConfigCommandId,
                     byteCommand(kMagicTransmitRate, static_cast<uint8_t>(rate), 0x00u));
}

Result<helpers::CanFrame> makeSetBaudAndShutdownDelayFrame(CanBaud baud,
                                                           std::chrono::milliseconds shutdownDelay)
{
    if (shutdownDelay < std::chrono::milliseconds { 0 })
    {
        return invalid_argument("shutdown delay cannot be negative");
    }

    if (shutdownDelay > std::chrono::milliseconds { 25500 })
    {
        return invalid_argument(fmt::format(
            "shutdown delay {}ms is longer than the 25500ms the device can store",
            shutdownDelay.count()));
    }

    if ((shutdownDelay.count() % 100) != 0)
    {
        return invalid_argument(fmt::format(
            "shutdown delay {}ms is not a whole number of tenths of a second, and the device "
            "stores only tenths -- pick a multiple of 100ms rather than have it truncated",
            shutdownDelay.count()));
    }

    const auto tenths = static_cast<uint8_t>(shutdownDelay.count() / 100);
    return makeFrame(kConfigCommandId,
                     byteCommand(kMagicBaudAndDelay, static_cast<uint8_t>(baud), tenths));
}

Result<helpers::CanFrame> makeSetOutputDriveFrame(OutputDrive drive)
{
    return makeFrame(kConfigCommandId,
                     byteCommand(kMagicOutputDrive, static_cast<uint8_t>(drive), 0x00u));
}

Result<helpers::CanFrame> makeSetCanShutdownFrame(CanKillMode mode, uint32_t killAddress)
{
    if (killAddress > kMaxStandardId)
    {
        return invalid_argument(fmt::format(
            "kill address 0x{:X} is wider than the 11 bits the device accepts", killAddress));
    }

    if (killAddress == kConfigCommandId)
    {
        return invalid_argument(fmt::format(
            "kill address 0x{:X} is the configuration identifier, so every configuration command "
            "would also read as a shutdown request",
            killAddress));
    }

    const auto word =
        static_cast<uint16_t>((static_cast<uint16_t>(mode) << 12) | static_cast<uint16_t>(killAddress));
    return makeFrame(kConfigCommandId, wordCommand(kMagicCanShutdown, word));
}

Result<helpers::CanFrame> makeCanKillTriggerFrame(uint32_t killAddress)
{
    if (killAddress > kMaxStandardId)
    {
        return invalid_argument(fmt::format(
            "kill address 0x{:X} is wider than the 11 bits the device accepts", killAddress));
    }

    // Manual table 11. Not a magic-and-value command like the others -- the
    // payload is the whole message, and it is constant.
    const std::array<uint8_t, kCommandLength> payload = { 0xFFu, 0x00u, 0xFFu, 0x00u,
                                                          0xFFu, 0x00u, 0xFFu, 0x00u };
    return makeFrame(killAddress, payload);
}

CommandEffect effectOf(const helpers::CanFrame& command)
{
    if (command.id != kConfigCommandId)
    {
        // The kill trigger, or something that is not ours at all. Neither waits
        // on a power cycle.
        return CommandEffect::Immediate;
    }

    switch (magicOf(command))
    {
    case kMagicBaseAddress:
        // The one command the manual describes as taking effect on acknowledge:
        // "the Master Relay will be configured to send all CAN information on
        // the new base address".
        return CommandEffect::Immediate;

    case kMagicBaudAndDelay:
        // Half of this command -- the shutdown delay -- is live immediately,
        // and the baud rate is not. Reported as needing a power cycle because
        // that is the answer that does not mislead: an operator who cycles the
        // relay gets both, one who does not gets only the delay.
        return CommandEffect::RequiresPowerCycle;

    case kMagicTransmitRate:
    case kMagicOutputDrive:
    case kMagicCanShutdown:
        return CommandEffect::RequiresPowerCycle;

    default:
        return CommandEffect::Immediate;
    }
}

std::optional<ConfigResponse> decodeConfigResponse(std::span<const uint8_t> data)
{
    if (data.size() != kCommandLength)
    {
        return std::nullopt;
    }

    const uint8_t first = data[0];
    for (const uint8_t byte : data)
    {
        if (byte != first)
        {
            return std::nullopt;
        }
    }

    switch (first)
    {
    case 0x00: return ConfigResponse::Success;
    case 0x11: return ConfigResponse::IdMismatch;
    case 0x22: return ConfigResponse::FrameCheckError;
    case 0x33: return ConfigResponse::InvalidId;
    default: return std::nullopt;
    }
}

std::string toHex(std::span<const uint8_t> data)
{
    std::string out;
    out.reserve(data.size() * 3u);
    for (const uint8_t byte : data)
    {
        if (!out.empty())
        {
            out += " ";
        }
        out += fmt::format("{:02X}", byte);
    }
    return out;
}

} // namespace msel
