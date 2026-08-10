// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands that reconfigure an MSEL Master Relay, checked against the byte
// sequences printed in the manual.
//
// This is the half of the device that cannot be verified by watching it: a
// malformed command is answered with an error code that looks much like the
// error code for a value the device disliked, and both look like nothing at all
// when the external kill switch is not held. The manual prints one worked
// example per command, and those examples are the only independent statement of
// what the bytes should be -- so every builder is pinned to its example here.
// Getting a magic word or a duplicated byte wrong is otherwise a mistake that
// only shows up on a car, as a relay that will not accept its settings.
//
// The other half of this file is refusals. Every builder rejects values the
// device cannot store, and the interesting cases are the ones where accepting
// them would silently do something else instead -- a shutdown delay of 150ms
// truncating to 100ms, a base address whose third message falls off the end of
// the 11-bit identifier space.

#include "msel/protocol.h"

#include <spdlog/spdlog.h>

#include <array>
#include <string>
#include <vector>

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

void checkFrame(const msel::Result<helpers::CanFrame>& actual,
                uint32_t expectedId,
                const std::array<uint8_t, 8>& expected,
                const std::string& what)
{
    if (!actual)
    {
        SPDLOG_ERROR("FAIL: {}: rejected with '{}'", what, msel::to_string(actual.error()));
        ++failures;
        return;
    }

    const auto& frame = *actual;
    if (frame.id != expectedId)
    {
        SPDLOG_ERROR("FAIL: {}: identifier 0x{:X}, expected 0x{:X}", what, frame.id, expectedId);
        ++failures;
        return;
    }

    if (frame.len != 8u)
    {
        SPDLOG_ERROR("FAIL: {}: length {}, expected 8", what, frame.len);
        ++failures;
        return;
    }

    if (frame.isExtended)
    {
        SPDLOG_ERROR("FAIL: {}: built an extended frame; the relay speaks 11-bit only", what);
        ++failures;
        return;
    }

    for (size_t i = 0u; i < 8u; ++i)
    {
        if (frame.data[i] != expected[i])
        {
            SPDLOG_ERROR("FAIL: {}: got [{}], expected [{}]", what,
                         msel::toHex(frame.data_span()), msel::toHex(expected));
            ++failures;
            return;
        }
    }
}

void checkRejected(const msel::Result<helpers::CanFrame>& actual, const std::string& what)
{
    if (actual)
    {
        SPDLOG_ERROR("FAIL: {}: was accepted, and should not have been (built [{}])", what,
                     msel::toHex(actual->data_span()));
        ++failures;
    }
}

// Manual table 12: "Shows the correct message format to change the address to
// 0x6E4." The value travels as a big-endian word, written twice.
void test_set_base_address_matches_manual()
{
    checkFrame(msel::makeSetBaseAddressFrame(0x6E4u), 0x789u,
               { 0x07u, 0x89u, 0x06u, 0xE4u, 0x06u, 0xE4u, 0x07u, 0x89u },
               "set base address 0x6E4 (manual table 12)");

    // A second address, to show the word really tracks the argument rather than
    // the example happening to match a constant.
    checkFrame(msel::makeSetBaseAddressFrame(0x123u), 0x789u,
               { 0x07u, 0x89u, 0x01u, 0x23u, 0x01u, 0x23u, 0x07u, 0x89u },
               "set base address 0x123");
}

// Manual table 14: "Shows the correct message format to set the transmit rate
// to 100Hz." Here the value is an 8-bit code duplicated into adjacent bytes,
// not a word -- a different shape from the command above, which is exactly the
// kind of thing that is easy to get wrong once and never notice.
void test_set_transmit_rate_matches_manual()
{
    checkFrame(msel::makeSetTransmitRateFrame(msel::TransmitRate::Hz100), 0x789u,
               { 0x0Du, 0xEFu, 0x0Au, 0x0Au, 0x00u, 0x00u, 0x0Du, 0xEFu },
               "set transmit rate 100Hz (manual table 14)");

    checkFrame(msel::makeSetTransmitRateFrame(msel::TransmitRate::Hz10), 0x789u,
               { 0x0Du, 0xEFu, 0x01u, 0x01u, 0x00u, 0x00u, 0x0Du, 0xEFu },
               "set transmit rate 10Hz");
}

// Manual table 17: "Shows the correct message format to set the Baud to 500Kbps
// and the shutdown delay to 1 second."
void test_set_baud_and_delay_matches_manual()
{
    checkFrame(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate500k,
                                                      std::chrono::milliseconds { 1000 }),
               0x789u, { 0x04u, 0x56u, 0x01u, 0x01u, 0x0Au, 0x0Au, 0x04u, 0x56u },
               "set 500kbps and 1.0s delay (manual table 17)");

    checkFrame(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate1M,
                                                      std::chrono::milliseconds { 0 }),
               0x789u, { 0x04u, 0x56u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x56u },
               "set 1Mbps and no delay");

    // The longest the field holds is 255 tenths.
    checkFrame(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate250k,
                                                      std::chrono::milliseconds { 25500 }),
               0x789u, { 0x04u, 0x56u, 0x02u, 0x02u, 0xFFu, 0xFFu, 0x04u, 0x56u },
               "set 250kbps and the longest storable delay");
}

// Manual table 20: "Shows the correct message format to set the PDM / PDU
// output drive to active low, low side."
void test_set_output_drive_matches_manual()
{
    checkFrame(msel::makeSetOutputDriveFrame(msel::OutputDrive::ActiveLowLowSide), 0x789u,
               { 0x0Au, 0xBCu, 0x06u, 0x06u, 0x00u, 0x00u, 0x0Au, 0xBCu },
               "set output drive active low, low side (manual table 20)");

    checkFrame(msel::makeSetOutputDriveFrame(msel::OutputDrive::ActiveHighHalfBridge), 0x789u,
               { 0x0Au, 0xBCu, 0x00u, 0x00u, 0x00u, 0x00u, 0x0Au, 0xBCu },
               "set output drive to the factory default");
}

// Manual table 23: "Shows the correct message format to enable CAN Shutdown on
// address 0x6E6." The mode goes in the top nibble of the word and the address
// in the remaining twelve bits: 0x1 and 0x6E6 pack to 0x16E6.
void test_set_can_shutdown_matches_manual()
{
    checkFrame(msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled, 0x6E6u), 0x789u,
               { 0x01u, 0x23u, 0x16u, 0xE6u, 0x16u, 0xE6u, 0x01u, 0x23u },
               "enable CAN shutdown on 0x6E6 (manual table 23)");

    checkFrame(msel::makeSetCanShutdownFrame(msel::CanKillMode::Disabled, 0x6E6u), 0x789u,
               { 0x01u, 0x23u, 0x06u, 0xE6u, 0x06u, 0xE6u, 0x01u, 0x23u },
               "disable CAN shutdown");

    // Mode 2 is the MoTeC accident data recorder, whose status message defaults
    // to 0x7B.
    checkFrame(msel::makeSetCanShutdownFrame(msel::CanKillMode::Adr, 0x7Bu), 0x789u,
               { 0x01u, 0x23u, 0x20u, 0x7Bu, 0x20u, 0x7Bu, 0x01u, 0x23u },
               "enable accident data recorder shutdown on 0x7B");
}

// Manual table 11. Not a command on 0x789 -- it goes to the kill address, and
// its payload is constant.
void test_kill_trigger_matches_manual()
{
    checkFrame(msel::makeCanKillTriggerFrame(0x6E6u), 0x6E6u,
               { 0xFFu, 0x00u, 0xFFu, 0x00u, 0xFFu, 0x00u, 0xFFu, 0x00u },
               "CAN kill trigger on 0x6E6 (manual table 11)");
}

void test_commands_refuse_what_the_device_cannot_store()
{
    // Wider than 11 bits.
    checkRejected(msel::makeSetBaseAddressFrame(0x800u), "base address 0x800");

    // Fits itself, but base+3 does not -- the switch-state message would land
    // at 0x802. This is the case a naive `base <= 0x7FF` check lets through.
    checkRejected(msel::makeSetBaseAddressFrame(0x7FFu), "base address 0x7FF (base+3 overflows)");
    checkRejected(msel::makeSetBaseAddressFrame(0x7FDu), "base address 0x7FD (base+3 overflows)");
    check(msel::makeSetBaseAddressFrame(0x7FCu).has_value(),
          "base address 0x7FC is the largest that fits and must be accepted");

    // A base whose span swallows the configuration identifier would leave the
    // relay unable to be reconfigured again.
    checkRejected(msel::makeSetBaseAddressFrame(0x789u), "base address equal to 0x789");
    checkRejected(msel::makeSetBaseAddressFrame(0x786u), "base address 0x786 (base+3 is 0x789)");
    check(msel::makeSetBaseAddressFrame(0x785u).has_value(),
          "base address 0x785 clears 0x789 and must be accepted");

    // Longer than the one-byte field of tenths.
    checkRejected(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate1M,
                                                         std::chrono::milliseconds { 25600 }),
                  "shutdown delay 25.6s");
    checkRejected(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate1M,
                                                         std::chrono::milliseconds { -100 }),
                  "negative shutdown delay");

    // Not a whole tenth. Accepting this would store 100ms while the caller
    // believed it had asked for 150ms, which is worse than refusing.
    checkRejected(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate1M,
                                                         std::chrono::milliseconds { 150 }),
                  "shutdown delay 150ms");

    checkRejected(msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled, 0x800u),
                  "kill address 0x800");
    // A kill address equal to the configuration identifier would make every
    // configuration command double as a shutdown request.
    checkRejected(msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled, 0x789u),
                  "kill address equal to 0x789");
    checkRejected(msel::makeCanKillTriggerFrame(0x1FFFu), "kill trigger on a 29-bit identifier");
}

// Only the base address is live on acknowledge. Everything else is stored and
// read at boot, and telling an operator otherwise means they drive away with
// settings that have not taken.
void test_command_effects()
{
    const auto effect = [](const msel::Result<helpers::CanFrame>& frame) {
        return msel::effectOf(*frame);
    };

    check(effect(msel::makeSetBaseAddressFrame(0x6E4u)) == msel::CommandEffect::Immediate,
          "base address takes effect on acknowledge");
    check(effect(msel::makeSetTransmitRateFrame(msel::TransmitRate::Hz100)) ==
              msel::CommandEffect::RequiresPowerCycle,
          "transmit rate needs a power cycle");
    check(effect(msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate500k,
                                                        std::chrono::milliseconds { 1000 })) ==
              msel::CommandEffect::RequiresPowerCycle,
          "baud rate needs a power cycle");
    check(effect(msel::makeSetOutputDriveFrame(msel::OutputDrive::ActiveLowLowSide)) ==
              msel::CommandEffect::RequiresPowerCycle,
          "output drive needs a power cycle");
    check(effect(msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled, 0x6E6u)) ==
              msel::CommandEffect::RequiresPowerCycle,
          "CAN shutdown needs a power cycle");
    check(effect(msel::makeCanKillTriggerFrame(0x6E6u)) == msel::CommandEffect::Immediate,
          "the kill trigger is not a stored setting");
}

// The response shares an identifier with the periodic status message, so this
// is the function that decides which of the two a frame is. Both directions
// matter: missing a response loses the answer to a command, and mistaking a
// status frame for a response invents one.
void test_config_response_recognition()
{
    struct Case
    {
        std::string_view name;
        std::vector<uint8_t> data;
        std::optional<msel::ConfigResponse> expected;
    };

    const std::vector<Case> cases = {
        { "success", { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
          msel::ConfigResponse::Success },
        { "ids do not match", { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 },
          msel::ConfigResponse::IdMismatch },
        { "frame check error", { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 },
          msel::ConfigResponse::FrameCheckError },
        { "invalid id", { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 },
          msel::ConfigResponse::InvalidId },

        // Uniform, but not one of the four documented codes.
        { "uniform but undocumented", { 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44 },
          std::nullopt },

        // One byte differs. A real status frame reading 0x00 everywhere except
        // a single sensor count must not be swallowed as a response.
        { "nearly uniform", { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }, std::nullopt },
        { "nearly uniform at the front", { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
          std::nullopt },

        // A plausible status frame: 12.56V out, 54.5A, 25.2C, no warnings,
        // status "normal". Nothing about it is uniform.
        { "ordinary status frame", { 0x04, 0xE4, 0x02, 0x21, 0x00, 0xFC, 0x00, 0x01 },
          std::nullopt },

        // Length is part of the shape. A short frame is not a response, and
        // treating it as one would answer a command that was never answered.
        { "too short", { 0x00, 0x00, 0x00, 0x00 }, std::nullopt },
        { "empty", {}, std::nullopt },
    };

    for (const auto& testCase : cases)
    {
        const auto actual = msel::decodeConfigResponse(testCase.data);
        check(actual == testCase.expected,
              std::string("config response recognition: ") + std::string(testCase.name));
    }

    // The property the whole scheme rests on: no documented response code is
    // also a valid status enumeration, so a status frame can never be uniform
    // in one of them. If a firmware ever adds status 0x11, this breaks.
    for (const uint8_t code : { 0x00u, 0x11u, 0x22u, 0x33u })
    {
        check(!msel::statusFromRaw(code).has_value(),
              "response code 0x" + std::to_string(code) + " must not be a valid status");
    }
}

// The packed configuration word, byte 4 and byte 5 of the info message. The
// round trip has to preserve values the manual does not document, because the
// device is entitled to report them and normalising them away would hide the
// fact that it is in a state we do not understand.
void test_config_word_round_trip()
{
    // Manual table 10 worked through: CAN kill enabled (1), 500kbps (1),
    // active low, low side (6), one second of delay (10 tenths).
    msel::Config config;
    config.canKillRaw = 1u;
    config.canKill = msel::CanKillMode::Enabled;
    config.baudRaw = 1u;
    config.baud = msel::CanBaud::Rate500k;
    config.outputDriveRaw = 6u;
    config.outputDrive = msel::OutputDrive::ActiveLowLowSide;
    config.shutdownDelay = std::chrono::milliseconds { 1000 };

    const uint16_t word = msel::encodeConfigWord(config);
    check(word == 0x560Au,
          "config word packs to 0x560A, got 0x" + std::to_string(word));

    const auto decoded = msel::decodeConfigWord(word);
    check(decoded.canKill == msel::CanKillMode::Enabled, "round trip preserves CAN kill mode");
    check(decoded.baud == msel::CanBaud::Rate500k, "round trip preserves baud");
    check(decoded.outputDrive == msel::OutputDrive::ActiveLowLowSide,
          "round trip preserves output drive");
    check(decoded.shutdownDelay == std::chrono::milliseconds { 1000 },
          "round trip preserves shutdown delay");

    // Every representable word must survive the round trip byte for byte.
    for (uint32_t raw = 0u; raw <= 0xFFFFu; ++raw)
    {
        const auto value = static_cast<uint16_t>(raw);
        if (msel::encodeConfigWord(msel::decodeConfigWord(value)) != value)
        {
            check(false, "config word 0x" + std::to_string(raw) + " does not round trip");
            break;
        }
    }

    // Output drive 3 and 7 are unassigned, so they decode to "raw byte, no
    // meaning" rather than to a neighbouring mode.
    const auto undocumented = msel::decodeConfigWord(0x0300u);
    check(!undocumented.outputDrive.has_value(),
          "output drive 3 has no documented meaning and must decode as absent");
    check(undocumented.outputDriveRaw == 3u, "the undocumented raw value is still reported");
}

void test_enum_conversions()
{
    check(msel::statusFromRaw(1) == msel::Status::Normal, "status 1 is normal");
    check(msel::statusFromRaw(10) == msel::Status::PowerOnReset, "status 10 is power on reset");
    check(!msel::statusFromRaw(0).has_value(), "status 0 is not assigned");
    check(!msel::statusFromRaw(11).has_value(), "status 11 is not assigned");

    check(msel::canBaudFromRaw(2) == msel::CanBaud::Rate250k, "baud 2 is 250kbps");
    check(!msel::canBaudFromRaw(3).has_value(), "baud 3 is not assigned");
    check(msel::toBitsPerSecond(msel::CanBaud::Rate500k) == 500000u, "500kbps is 500000 bit/s");

    check(!msel::outputDriveFromRaw(3).has_value(), "output drive 3 is not assigned");
    check(!msel::outputDriveFromRaw(7).has_value(), "output drive 7 is not assigned");

    check(msel::switchStateFromRaw(4) == msel::SwitchState::LegacyCalInternalExternalSwitch,
          "switch state 4 decodes");
    check(!msel::switchStateFromRaw(5).has_value(), "switch state 5 is not assigned");

    check(msel::transmitRateFromRaw(0x0A) == msel::TransmitRate::Hz100, "0x0A is 100Hz");
    check(!msel::transmitRateFromRaw(0x02).has_value(), "0x02 is not a transmit rate");

    // The warnings byte is a mask, and several bits are set at once when
    // several conditions hold. The manual's own example: external switch and
    // driver switch off together give 0x60.
    const auto both = static_cast<uint8_t>(static_cast<uint8_t>(msel::Warning::ExternalSwitchKill) |
                                           static_cast<uint8_t>(msel::Warning::DriverSwitchKill));
    check(both == 0x60u, "external and driver switch kills combine to 0x60");
    check(msel::describeWarnings(both) == "driver switch kill|external switch kill",
          "describeWarnings lists every set bit");
    check(msel::describeWarnings(0u).empty(), "no warnings describes as nothing");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_set_base_address_matches_manual();
    test_set_transmit_rate_matches_manual();
    test_set_baud_and_delay_matches_manual();
    test_set_output_drive_matches_manual();
    test_set_can_shutdown_matches_manual();
    test_kill_trigger_matches_manual();
    test_commands_refuse_what_the_device_cannot_store();
    test_command_effects();
    test_config_response_recognition();
    test_config_word_round_trip();
    test_enum_conversions();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all MSEL protocol checks passed");
    return 0;
}
