// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MSEL Master Relay wire protocol: what the device says, and what it can be
// told. Pure functions over bytes -- nothing here opens a socket, touches
// zenoh, or keeps state. Building a frame and sending one are separate jobs, so
// that a dry run and a real write go through exactly the same code.
//
// Authored from the Master Relay User's Manual Rev 2.0 (September 2025). Table
// numbers in the comments below refer to it.
//
// Three parts of the protocol live here rather than in the DBC, because a DBC
// cannot describe them:
//
//   - The five configuration commands. All are sent to one fixed identifier
//     (0x789) and are told apart by a magic word repeated at both ends of the
//     payload, with each value carried twice. Nothing about that is a signal
//     layout.
//   - The remote kill trigger, which is eight constant bytes.
//   - The response to a configuration command, which arrives on the *base
//     status identifier* -- a second layout on an identifier that already
//     carries a periodic message.
//
// A caution that shapes the whole API: every one of these commands only takes
// effect while a human is pressing and holding the external kill switch on the
// relay. There is no software substitute, and the device will silently ignore a
// command sent without it. See requiresHeldExternalKillSwitch().
#ifndef MSEL_PROTOCOL_H
#define MSEL_PROTOCOL_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "helpers/can_frame.h"
#include "msel/error.h"

namespace msel
{

// Device state, as reported in byte 7 of the status message and in both
// shutdown-cause nibbles of the info message. Manual table 8.
enum class Status : uint8_t
{
    Normal = 1,
    OverTemperatureWarning = 2,
    OverCurrentWarning = 3,
    LowVoltageWarning = 4,
    HighVoltageWarning = 5,
    OverTemperatureKill = 6,
    DriverSwitchKill = 7,
    ExternalSwitchKill = 8,
    CanTriggerKill = 9,
    PowerOnReset = 10,
};

// Byte 6 of the status message is a bitmask, not an enumeration: several of
// these are set at once when several conditions hold. Manual table 8.
enum class Warning : uint8_t
{
    OverTemperature = 0x01,
    OverCurrent = 0x02,
    LowVoltage = 0x04,
    HighVoltage = 0x08,
    OverTemperatureKill = 0x10,
    DriverSwitchKill = 0x20,
    ExternalSwitchKill = 0x40,
    CanTriggerKill = 0x80,
};

// Manual table 18. These are the codes used in the *command*; the readback in
// the info message uses the same numbering.
enum class CanBaud : uint8_t
{
    Rate1M = 0,
    Rate500k = 1,
    Rate250k = 2,
};

// Manual tables 2 and 21. Note the gap: 3 is not assigned, and neither is 7.
enum class OutputDrive : uint8_t
{
    ActiveHighHalfBridge = 0,
    ActiveHighHighSide = 1,
    ActiveHighLowSide = 2,
    ActiveLowHalfBridge = 4,
    ActiveLowHighSide = 5,
    ActiveLowLowSide = 6,
};

// Manual table 24. `Adr` makes the relay listen for a MoTeC Accident Data
// Recorder's severe-event message instead of a plain kill frame.
enum class CanKillMode : uint8_t
{
    Disabled = 0,
    Enabled = 1,
    Adr = 2,
};

// Manual table 9. Only transmitted by units at serial number 64666 or above.
enum class SwitchState : uint8_t
{
    NormalCalNormalExternalSwitch = 1,
    NormalCalInternalExternalSwitch = 2,
    LegacyCalNormalExternalSwitch = 3,
    LegacyCalInternalExternalSwitch = 4,
};

// Manual table 15. The value is the code sent in the command, not the rate.
enum class TransmitRate : uint8_t
{
    Hz10 = 0x01,
    Hz100 = 0x0A,
};

// What the relay answers to a configuration command, as eight identical bytes.
// Manual tables 13, 16, 19, 22 and 25 -- which are the same four codes each
// time.
enum class ConfigResponse : uint8_t
{
    Success = 0x00,
    IdMismatch = 0x11,
    FrameCheckError = 0x22,
    InvalidId = 0x33,
};

const char* to_string(Status value);
const char* to_string(Warning value);
const char* to_string(CanBaud value);
const char* to_string(OutputDrive value);
const char* to_string(CanKillMode value);
const char* to_string(SwitchState value);
const char* to_string(TransmitRate value);
const char* to_string(ConfigResponse value);

// The bus rate each CanBaud names, for logging and for cross-checking against
// whatever the CAN bridge is actually running at.
uint32_t toBitsPerSecond(CanBaud value);

// Raw-to-enum conversions. Every one returns nullopt for a value the manual
// does not assign, rather than producing an enumerator that does not exist:
// the device is the source of these bytes, and a firmware we do not know about
// is exactly the case where guessing turns into a confidently wrong readout.
std::optional<Status> statusFromRaw(uint8_t raw);
std::optional<CanBaud> canBaudFromRaw(uint8_t raw);
std::optional<OutputDrive> outputDriveFromRaw(uint8_t raw);
std::optional<CanKillMode> canKillModeFromRaw(uint8_t raw);
std::optional<SwitchState> switchStateFromRaw(uint8_t raw);
std::optional<TransmitRate> transmitRateFromRaw(uint8_t raw);

// Renders a warnings bitmask as a human-readable list, e.g.
// "over current|driver switch kill". Empty string when nothing is set.
std::string describeWarnings(uint8_t mask);

// The identifier the five configuration commands are sent to. Fixed in
// firmware: unlike the status messages, it does not move with the base address.
inline constexpr uint32_t kConfigCommandId = 0x789u;

// The factory default base address, and the largest an 11-bit identifier goes.
inline constexpr uint32_t kDefaultBaseAddress = 0x6E4u;
inline constexpr uint32_t kDefaultKillAddress = 0x6E6u;
inline constexpr uint32_t kMaxStandardId = 0x7FFu;

// Where this relay's three periodic messages sit.
//
// The base address is user-configurable, so these are not constants -- which is
// the single most important thing to know about decoding this device. The DBC
// describes the messages at their factory identifiers; everything that receives
// frames maps an observed identifier back onto the canonical one through here.
struct Addresses
{
    uint32_t base { kDefaultBaseAddress };

    constexpr uint32_t status() const { return base; }
    constexpr uint32_t info() const { return base + 1u; }
    // Base + 2 is not used by any firmware the manual describes.
    constexpr uint32_t switchState() const { return base + 3u; }
};

// Rejects a base address the relay could not actually use: one that does not
// fit in 11 bits once base+3 is taken into account, or one whose span would
// swallow the fixed configuration identifier and make the device unreachable
// for further changes.
Result<void> validateBaseAddress(uint32_t base);

// The relay's stored settings, as read back in the info message and as written
// by the configuration commands.
//
// The three enum fields are optional for the reason given on the *FromRaw
// functions: a value outside the documented set is reported as absent with the
// raw byte preserved, not silently coerced onto a neighbouring meaning.
struct Config
{
    std::optional<CanKillMode> canKill;
    uint8_t canKillRaw { 0u };

    std::optional<CanBaud> baud;
    uint8_t baudRaw { 0u };

    std::optional<OutputDrive> outputDrive;
    uint8_t outputDriveRaw { 0u };

    // Held-up time after a shutdown event, so loggers can flush. Encoded in
    // tenths of a second, so this is always a whole multiple of 100ms.
    std::chrono::milliseconds shutdownDelay { 0 };
};

// Bytes 4 and 5 of the info message, packed as byte4 << 8 | byte5. Manual
// table 10.
Config decodeConfigWord(uint16_t word);
uint16_t encodeConfigWord(const Config& config);

// Decoded periodic messages. These carry both the typed value and the raw byte
// wherever the device could report something undocumented, so a caller can show
// what actually arrived instead of "unknown".
struct StatusFrame
{
    double voltageOut { 0.0 };           // V, on the output terminal
    double loadCurrent { 0.0 };          // A, positive when discharging
    double temperatureInternal { 0.0 };  // degrees C
    uint8_t warnings { 0u };             // bitmask of Warning
    uint8_t statusRaw { 0u };
    std::optional<Status> status;
};

struct InfoFrame
{
    double voltageIn { 0.0 };            // V, on the input (battery) terminal
    uint16_t serialNo { 0u };
    Config config;
    std::chrono::milliseconds timeSinceShutdown { 0 };
    uint8_t shutdownCauseRaw { 0u };
    std::optional<Status> shutdownCause;
    uint8_t shutdownCause2Raw { 0u };
    std::optional<Status> shutdownCause2;
};

struct SwitchStateFrame
{
    uint8_t switchStateRaw { 0u };
    std::optional<SwitchState> switchState;
};

// Whether the device has to be power-cycled before a command takes effect.
// Only the shutdown delay applies immediately; everything else is stored and
// read at boot. Returning this rather than logging it lets the node hand the
// fact back to whoever asked for the change.
enum class CommandEffect
{
    Immediate,
    RequiresPowerCycle,
};

// Every configuration command needs the external kill switch held down while it
// is transmitted. This is a function rather than a constant so that call sites
// read as a statement about the protocol, and so there is one place to change
// if a future firmware relaxes it.
constexpr bool requiresHeldExternalKillSwitch()
{
    return true;
}

// Command builders.
//
// Each returns the frame to transmit, or an error describing what the caller
// asked for that the device cannot be told. None of them send anything.

// Moves all three periodic messages to a new base address. Manual table 12.
Result<helpers::CanFrame> makeSetBaseAddressFrame(uint32_t newBase);

// 10Hz or 100Hz. Manual tables 14 and 15.
Result<helpers::CanFrame> makeSetTransmitRateFrame(TransmitRate rate);

// Bus rate and shutdown delay travel in one command. Manual tables 17 and 18.
//
// The delay is rejected rather than rounded when it is not a whole multiple of
// 100ms: the field is in tenths of a second, and quietly truncating 150ms to
// 100ms would leave the caller believing it had set something it had not.
Result<helpers::CanFrame> makeSetBaudAndShutdownDelayFrame(CanBaud baud,
                                                           std::chrono::milliseconds shutdownDelay);

// PDM/ECU output drive. Manual tables 20 and 21.
Result<helpers::CanFrame> makeSetOutputDriveFrame(OutputDrive drive);

// Enables or disables remote shutdown, and sets the identifier it listens on.
// Manual tables 23 and 24.
Result<helpers::CanFrame> makeSetCanShutdownFrame(CanKillMode mode, uint32_t killAddress);

// The frame that actually shuts the vehicle down, once the above has enabled
// it. Manual table 11. Eight constant bytes on the configured kill address.
Result<helpers::CanFrame> makeCanKillTriggerFrame(uint32_t killAddress);

// What a given command needs before it is live.
CommandEffect effectOf(const helpers::CanFrame& command);

// Recognises the relay's answer to a configuration command.
//
// This is the awkward corner of the protocol: the answer arrives on the base
// status identifier, which already carries a periodic message at 10Hz. The two
// are told apart by shape. A response is eight bytes that are all equal and all
// one of the four documented codes; a status frame cannot look like that,
// because its byte 7 is a Status enumeration and every valid Status is in 1..10
// while every response code (0x00, 0x11, 0x22, 0x33) is outside that range.
// That non-overlap is the whole reason this function is safe, so it must be
// rechecked if either set ever grows.
//
// Returns nullopt for anything that is not unambiguously a response, which
// includes a short frame -- the caller then treats the frame as periodic data.
std::optional<ConfigResponse> decodeConfigResponse(std::span<const uint8_t> data);

// Hex rendering of a frame's payload, for logging and for handing back to a
// caller that wants to see exactly which bytes went out.
std::string toHex(std::span<const uint8_t> data);

} // namespace msel

#endif // MSEL_PROTOCOL_H
