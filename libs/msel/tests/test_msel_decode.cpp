// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoding an MSEL Master Relay: what comes off the bus, and what must not.
//
// Three things here are worth more than the happy path.
//
// The first is re-addressing. The relay's base CAN address is configurable, so
// its three messages are not at fixed identifiers, and a decoder that gets this
// wrong is wrong in the worst available way -- it publishes plausible voltages
// and temperatures for frames belonging to some other device. The tests below
// drive the decoder at a shifted base and then feed it traffic chosen to alias
// if the implementation had subtracted an offset from every frame it saw.
//
// The second is the collision between a configuration response and the periodic
// status message, which share an identifier. Both directions are checked: a
// response must not be decoded as telemetry, and telemetry must not be reported
// as an answer to a command that is still outstanding.
//
// The third is the round trip against StubRelay. That stub packs its frames by
// hand from the manual's byte tables rather than through the DBC, so decoding
// them is a real check that the DBC's signal layout matches the hardware --
// which encoding and decoding through the same generated code would not be.

#include "msel/decoder.h"
#include "msel/stub_relay.h"

#include "dbc_msel_master_relay.h"

#include <spdlog/spdlog.h>

#include <cmath>
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

void checkNear(double actual, double expected, double tolerance, const std::string& what)
{
    if (std::fabs(actual - expected) > tolerance)
    {
        SPDLOG_ERROR("FAIL: {}: got {}, expected {}", what, actual, expected);
        ++failures;
    }
}

helpers::CanFrame frameOf(uint32_t id, std::vector<uint8_t> bytes)
{
    helpers::CanFrame frame;
    frame.id = id;
    frame.len = static_cast<uint8_t>(bytes.size());
    for (size_t i = 0u; i < bytes.size() && i < frame.data.size(); ++i)
    {
        frame.data[i] = bytes[i];
    }
    return frame;
}

// The manual's own worked numbers: 12.56V, 54.5A, 25.2C. 1256 is 0x04E8, 545 is
// 0x0221, 252 is 0x00FC.
helpers::CanFrame statusFrameAt(uint32_t id)
{
    return frameOf(id, { 0x04, 0xE8, 0x02, 0x21, 0x00, 0xFC, 0x00, 0x01 });
}

void test_status_decodes()
{
    msel::Decoder decoder;
    msel::StatusFrame seen;
    int calls = 0;
    decoder.onStatus([&](const msel::StatusFrame& frame) {
        seen = frame;
        ++calls;
    });

    check(decoder.onFrame(statusFrameAt(0x6E4u)) == msel::Decoder::Accepted::Status,
          "a status frame at the default base is accepted");
    check(calls == 1, "the status callback fires once per frame");

    checkNear(seen.voltageOut, 12.56, 0.001, "voltage out");
    checkNear(seen.loadCurrent, 54.5, 0.001, "load current");
    checkNear(seen.temperatureInternal, 25.2, 0.001, "internal temperature");
    check(seen.warnings == 0u, "no warnings set");
    check(seen.status == msel::Status::Normal, "status decodes as normal");
    check(decoder.snapshot().status.has_value(), "the snapshot keeps the last status");

    // Current is signed: the relay reports a negative value while the battery
    // is being charged. -54.5A is -545, which is 0xFDDF.
    decoder.onFrame(frameOf(0x6E4u, { 0x04, 0xE8, 0xFD, 0xDF, 0x00, 0xFC, 0x00, 0x01 }));
    checkNear(seen.loadCurrent, -54.5, 0.001, "load current is signed when charging");

    // Temperature is signed too, and a cold car is a real case: -10.0C is -100,
    // which is 0xFF9C.
    decoder.onFrame(frameOf(0x6E4u, { 0x04, 0xE8, 0x02, 0x21, 0xFF, 0x9C, 0x00, 0x01 }));
    checkNear(seen.temperatureInternal, -10.0, 0.001, "internal temperature is signed");

    // All eight warning bits at once, and a status the manual does not assign.
    decoder.onFrame(frameOf(0x6E4u, { 0x04, 0xE8, 0x02, 0x21, 0x00, 0xFC, 0xFF, 0x7F }));
    check(seen.warnings == 0xFFu, "every warning bit is carried through");
    check(seen.statusRaw == 0x7Fu, "an undocumented status keeps its raw byte");
    check(!seen.status.has_value(), "an undocumented status decodes as absent, not as a guess");
}

void test_info_decodes()
{
    msel::Decoder decoder;
    msel::InfoFrame seen;
    decoder.onInfo([&](const msel::InfoFrame& frame) { seen = frame; });

    // 12.56V in, serial 64666 (0xFC9A), config word 0x560A (CAN kill enabled,
    // 500kbps, active low low side, 1.0s), 1.5s since shutdown, last shutdown
    // was a driver switch kill (7) and the one before an external switch kill
    // (8).
    check(decoder.onFrame(frameOf(0x6E5u, { 0x04, 0xE8, 0xFC, 0x9A, 0x56, 0x0A, 0x0F, 0x87 })) ==
              msel::Decoder::Accepted::Info,
          "an info frame at the default base is accepted");

    checkNear(seen.voltageIn, 12.56, 0.001, "voltage in");
    check(seen.serialNo == 64666u, "serial number");
    check(seen.config.canKill == msel::CanKillMode::Enabled, "config CAN kill mode");
    check(seen.config.baud == msel::CanBaud::Rate500k, "config baud");
    check(seen.config.outputDrive == msel::OutputDrive::ActiveLowLowSide, "config output drive");
    check(seen.config.shutdownDelay == std::chrono::milliseconds { 1000 },
          "config shutdown delay is exactly 1000ms, not 999");
    check(seen.timeSinceShutdown == std::chrono::milliseconds { 1500 }, "time since shutdown");
    check(seen.shutdownCause == msel::Status::DriverSwitchKill,
          "the low nibble is the most recent shutdown");
    check(seen.shutdownCause2 == msel::Status::ExternalSwitchKill,
          "the high nibble is the one before it");

    // A relay that has never shut down reports cause 0, which is not a status.
    decoder.onFrame(frameOf(0x6E5u, { 0x04, 0xE8, 0xFC, 0x9A, 0x56, 0x0A, 0x00, 0x00 }));
    check(!seen.shutdownCause.has_value(), "cause 0 means no shutdown yet, not a status");
    check(seen.shutdownCauseRaw == 0u, "the raw cause byte is still reported");
}

void test_switch_state_decodes()
{
    msel::Decoder decoder;
    msel::SwitchStateFrame seen;
    decoder.onSwitchState([&](const msel::SwitchStateFrame& frame) { seen = frame; });

    check(decoder.onFrame(frameOf(0x6E7u, { 0x00, 0x02 })) ==
              msel::Decoder::Accepted::SwitchState,
          "a two-byte switch-state frame is accepted");
    check(seen.switchState == msel::SwitchState::NormalCalInternalExternalSwitch,
          "switch state decodes");

    // The manual documents two bytes but a device sending a full eight must
    // still decode -- which is why the DBC declares this message as two.
    check(decoder.onFrame(frameOf(0x6E7u, { 0x00, 0x03, 0, 0, 0, 0, 0, 0 })) ==
              msel::Decoder::Accepted::SwitchState,
          "a longer switch-state frame still decodes");
    check(seen.switchState == msel::SwitchState::LegacyCalNormalExternalSwitch,
          "the longer frame decodes to the same field");
}

// The important one. A relay moved to a new base address must decode there, and
// must not drag unrelated traffic along with it.
void test_base_address_rebasing()
{
    msel::Decoder decoder(msel::Addresses { .base = 0x500u });

    check(decoder.onFrame(statusFrameAt(0x500u)) == msel::Decoder::Accepted::Status,
          "status decodes at the configured base");
    check(decoder.onFrame(frameOf(0x501u, { 0x04, 0xE8, 0xFC, 0x9A, 0x56, 0x0A, 0x00, 0x00 })) ==
              msel::Decoder::Accepted::Info,
          "info decodes at base+1");
    check(decoder.onFrame(frameOf(0x503u, { 0x00, 0x01 })) ==
              msel::Decoder::Accepted::SwitchState,
          "switch state decodes at base+3");

    // The factory identifiers are now somebody else's problem, and must not
    // decode. A decoder that matched the DBC's identifiers directly would
    // accept these.
    check(decoder.onFrame(statusFrameAt(0x6E4u)) == msel::Decoder::Accepted::No,
          "the factory base does not decode once the relay has been moved");
    check(decoder.onFrame(frameOf(0x6E5u, { 0x04, 0xE8, 0xFC, 0x9A, 0x56, 0x0A, 0x00, 0x00 })) ==
              msel::Decoder::Accepted::No,
          "the factory info identifier does not decode either");

    // base+2 is not used by any documented firmware.
    check(decoder.onFrame(frameOf(0x502u, { 0x04, 0xE8, 0x02, 0x21, 0x00, 0xFC, 0x00, 0x01 })) ==
              msel::Decoder::Accepted::No,
          "base+2 is unassigned and must not decode");

    // The aliasing case. If the implementation subtracted (base - 0x6E4) from
    // every incoming identifier and handed the result to the DBC, then with a
    // base of 0x500 the offset is -0x1E4 and an unrelated frame at 0x6E4+0x1E4
    // would map onto the status message. Nothing on a shared bus should be able
    // to impersonate the relay.
    check(decoder.onFrame(statusFrameAt(0x8C8u)) == msel::Decoder::Accepted::No,
          "an unrelated identifier does not alias onto the status message");
    check(decoder.onFrame(statusFrameAt(0x4FFu)) == msel::Decoder::Accepted::No,
          "an identifier just below the base does not decode");
    check(decoder.onFrame(statusFrameAt(0x504u)) == msel::Decoder::Accepted::No,
          "an identifier just above the last message does not decode");
}

// Regression. Following the relay to a new base address used to be done by
// assigning a freshly built Decoder over the old one, which threw away every
// registered callback. The symptom was as bad as it gets: the change succeeded,
// frames arrived at the new address, they decoded, the snapshot updated -- and
// nothing was published ever again. Nothing logged, nothing failed.
void test_changing_the_base_address_keeps_the_callbacks()
{
    msel::Decoder decoder;
    int statusCalls = 0;
    int infoCalls = 0;
    int switchStateCalls = 0;
    int responseCalls = 0;

    decoder.onStatus([&](const msel::StatusFrame&) { ++statusCalls; });
    decoder.onInfo([&](const msel::InfoFrame&) { ++infoCalls; });
    decoder.onSwitchState([&](const msel::SwitchStateFrame&) { ++switchStateCalls; });
    decoder.onConfigResponse([&](msel::ConfigResponse) { ++responseCalls; });

    check(decoder.onFrame(statusFrameAt(0x6E4u)) == msel::Decoder::Accepted::Status,
          "decodes at the factory base to begin with");
    check(statusCalls == 1, "and the callback fires");

    decoder.setAddresses(msel::Addresses { .base = 0x500u });
    check(decoder.addresses().base == 0x500u, "the decoder moved");

    // The whole point: every callback still has to be attached.
    check(decoder.onFrame(statusFrameAt(0x500u)) == msel::Decoder::Accepted::Status,
          "decodes at the new base");
    check(statusCalls == 2, "the status callback survived the move");

    decoder.onFrame(frameOf(0x501u, { 0x04, 0xE8, 0xFC, 0x9A, 0x56, 0x0A, 0x00, 0x00 }));
    check(infoCalls == 1, "the info callback survived the move");

    decoder.onFrame(frameOf(0x503u, { 0x00, 0x01 }));
    check(switchStateCalls == 1, "the switch-state callback survived the move");

    decoder.onFrame(frameOf(0x500u, { 0, 0, 0, 0, 0, 0, 0, 0 }));
    check(responseCalls == 1, "the config-response callback survived the move");

    // And the old identifiers stop decoding, so a relay left at the factory
    // address does not keep feeding stale readings after the move.
    check(decoder.onFrame(statusFrameAt(0x6E4u)) == msel::Decoder::Accepted::No,
          "the old base no longer decodes");
    check(statusCalls == 2, "and fires nothing");

    // The snapshot is kept across the move: it is the same physical relay, and
    // dropping it would make a settings read-back claim it had never been heard
    // from a moment after successfully re-addressing it.
    check(decoder.snapshot().info.has_value(),
          "the snapshot survives the move");
}

void test_malformed_frames_are_rejected()
{
    msel::Decoder decoder;
    int statusCalls = 0;
    decoder.onStatus([&](const msel::StatusFrame&) { ++statusCalls; });

    // Short of the eight bytes the status message needs. Decoding it would
    // read the zero padding as readings.
    check(decoder.onFrame(frameOf(0x6E4u, { 0x04, 0xE8, 0x02 })) == msel::Decoder::Accepted::No,
          "a short status frame is rejected");
    check(statusCalls == 0, "a rejected frame does not fire the callback");
    check(!decoder.snapshot().status.has_value(), "a rejected frame does not reach the snapshot");

    // Same number, different message: a 29-bit identifier that happens to read
    // 0x6E4 is a different address on the bus.
    auto extended = statusFrameAt(0x6E4u);
    extended.isExtended = true;
    check(decoder.onFrame(extended) == msel::Decoder::Accepted::No,
          "an extended frame with the same number is not ours");

    // A remote transmission request carries no payload.
    auto remote = statusFrameAt(0x6E4u);
    remote.isRTR = true;
    check(decoder.onFrame(remote) == msel::Decoder::Accepted::No,
          "a remote request is not a status message");

    // An error frame's identifier is a bitmap of bus conditions, not an
    // address.
    auto error = statusFrameAt(0x6E4u);
    error.isError = true;
    check(decoder.onFrame(error) == msel::Decoder::Accepted::No,
          "a controller error frame is not a status message");

    check(statusCalls == 0, "none of the malformed frames decoded");
}

// The collision. Both of these arrive on the base identifier.
void test_config_response_and_status_share_an_identifier()
{
    msel::Decoder decoder;
    int statusCalls = 0;
    int responseCalls = 0;
    msel::ConfigResponse lastResponse = msel::ConfigResponse::InvalidId;

    decoder.onStatus([&](const msel::StatusFrame&) { ++statusCalls; });
    decoder.onConfigResponse([&](msel::ConfigResponse response) {
        lastResponse = response;
        ++responseCalls;
    });

    check(decoder.onFrame(frameOf(0x6E4u, { 0, 0, 0, 0, 0, 0, 0, 0 })) ==
              msel::Decoder::Accepted::ConfigResponse,
          "an all-zero frame on the base identifier is a success response");
    check(responseCalls == 1 && lastResponse == msel::ConfigResponse::Success,
          "the response reaches the response callback");
    check(statusCalls == 0, "and not the status callback");
    check(decoder.snapshot().lastConfigResponse == msel::ConfigResponse::Success,
          "the snapshot keeps the last response");

    check(decoder.onFrame(frameOf(0x6E4u, { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 })) ==
              msel::Decoder::Accepted::ConfigResponse,
          "an invalid-id response is recognised");
    check(lastResponse == msel::ConfigResponse::InvalidId, "and carries its code");

    // Now the other direction: ordinary telemetry must still get through.
    check(decoder.onFrame(statusFrameAt(0x6E4u)) == msel::Decoder::Accepted::Status,
          "a status frame on the same identifier still decodes as telemetry");
    check(statusCalls == 1, "the status callback fired");
    check(responseCalls == 2, "and no extra response was invented");
}

// StubRelay packs its frames independently of the DBC, so this is the test that
// says the DBC's signal layout is right.
void test_stub_relay_round_trip()
{
    msel::StubRelay relay;
    relay.state().voltageIn = 13.80;
    relay.state().voltageOut = 13.75;
    relay.state().loadCurrent = -12.3;
    relay.state().temperatureInternal = 47.6;
    relay.state().warnings = static_cast<uint8_t>(msel::Warning::LowVoltage);
    relay.state().statusRaw = static_cast<uint8_t>(msel::Status::LowVoltageWarning);
    relay.state().serialNo = 12345u;
    relay.state().timeSinceShutdown = std::chrono::milliseconds { 2500 };
    relay.state().shutdownCauseRaw = static_cast<uint8_t>(msel::Status::DriverSwitchKill);
    relay.state().shutdownCause2Raw = static_cast<uint8_t>(msel::Status::OverCurrentWarning);
    relay.state().switchStateRaw =
        static_cast<uint8_t>(msel::SwitchState::NormalCalInternalExternalSwitch);

    msel::Decoder decoder(relay.addresses());
    for (const auto& frame : relay.periodicFrames())
    {
        check(decoder.onFrame(frame) != msel::Decoder::Accepted::No,
              "every frame the relay transmits decodes");
    }

    const auto& snapshot = decoder.snapshot();
    check(snapshot.status.has_value() && snapshot.info.has_value() &&
              snapshot.switchState.has_value(),
          "one transmission cycle fills all three messages");

    checkNear(snapshot.status->voltageOut, 13.75, 0.005, "round trip: voltage out");
    checkNear(snapshot.status->loadCurrent, -12.3, 0.05, "round trip: load current");
    checkNear(snapshot.status->temperatureInternal, 47.6, 0.05, "round trip: temperature");
    check(snapshot.status->warnings == static_cast<uint8_t>(msel::Warning::LowVoltage),
          "round trip: warnings mask");
    check(snapshot.status->status == msel::Status::LowVoltageWarning, "round trip: status");

    checkNear(snapshot.info->voltageIn, 13.80, 0.005, "round trip: voltage in");
    check(snapshot.info->serialNo == 12345u, "round trip: serial number");
    check(snapshot.info->timeSinceShutdown == std::chrono::milliseconds { 2500 },
          "round trip: time since shutdown");
    check(snapshot.info->shutdownCause == msel::Status::DriverSwitchKill,
          "round trip: last shutdown cause");
    check(snapshot.info->shutdownCause2 == msel::Status::OverCurrentWarning,
          "round trip: previous shutdown cause");
    check(snapshot.info->config.baud == msel::CanBaud::Rate1M,
          "round trip: factory baud is 1Mbps");
    check(snapshot.info->config.shutdownDelay == std::chrono::milliseconds { 1000 },
          "round trip: factory shutdown delay is 1.0s");
    check(snapshot.switchState->switchState == msel::SwitchState::NormalCalInternalExternalSwitch,
          "round trip: switch state");

    // An older unit sends only two messages, and that is not a fault.
    msel::StubRelay older;
    older.state().transmitsSwitchState = false;
    check(older.periodicFrames().size() == 2u,
          "a relay below serial 64666 transmits two messages, not three");
}

// The stub is the only place the hold-the-switch requirement can be exercised,
// and it is the requirement most likely to be forgotten by anyone using this
// library.
void test_commands_need_the_external_kill_switch()
{
    msel::StubRelay relay;
    const auto command = msel::makeSetOutputDriveFrame(msel::OutputDrive::ActiveLowLowSide);
    check(command.has_value(), "the output drive command builds");

    check(!relay.onFrame(*command).has_value(),
          "a command sent without the external kill switch held gets no answer at all");
    check(!relay.hasPendingConfig(), "and changes nothing");

    relay.setExternalKillSwitchHeld(true);
    const auto response = relay.onFrame(*command);
    check(response.has_value(), "with the switch held the relay answers");
    check(msel::decodeConfigResponse(response->data_span()) == msel::ConfigResponse::Success,
          "and the answer is success");
    check(response->id == relay.addresses().status(),
          "the answer comes back on the base identifier, not on 0x789");

    // Stored, but not live. This is what "requires a power cycle" means in
    // practice, and reporting it wrongly sends someone away with settings that
    // have not taken.
    check(relay.hasPendingConfig(), "the setting is stored");
    check(relay.state().config.outputDrive == msel::OutputDrive::ActiveHighHalfBridge,
          "but the relay still reports the old output drive");

    relay.powerCycle();
    check(relay.state().config.outputDrive == msel::OutputDrive::ActiveLowLowSide,
          "after a power cycle the new output drive is live");
    check(!relay.hasPendingConfig(), "and nothing is left pending");
}

void test_relay_rejects_bad_commands()
{
    msel::StubRelay relay;
    relay.setExternalKillSwitchHeld(true);

    const auto responseTo = [&relay](std::vector<uint8_t> bytes) {
        const auto reply = relay.onFrame(frameOf(msel::kConfigCommandId, std::move(bytes)));
        return reply ? msel::decodeConfigResponse(reply->data_span()) : std::nullopt;
    };

    // Magic at the front, something else at the back.
    check(responseTo({ 0x0A, 0xBC, 0x06, 0x06, 0x00, 0x00, 0x0A, 0xBD }) ==
              msel::ConfigResponse::FrameCheckError,
          "a payload whose magic words disagree is a frame check error");

    // The two copies of the value disagree.
    check(responseTo({ 0x0A, 0xBC, 0x06, 0x05, 0x00, 0x00, 0x0A, 0xBC }) ==
              msel::ConfigResponse::IdMismatch,
          "a value that is not duplicated consistently is an id mismatch");

    // Well formed, but 3 is not an assigned output drive.
    check(responseTo({ 0x0A, 0xBC, 0x03, 0x03, 0x00, 0x00, 0x0A, 0xBC }) ==
              msel::ConfigResponse::InvalidId,
          "an unassigned output drive is an invalid id");

    // A magic word that is not one of the five.
    check(responseTo({ 0xDE, 0xAD, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD }) ==
              msel::ConfigResponse::FrameCheckError,
          "an unrecognised command is a frame check error");

    // Every frame the builders produce must be one the relay accepts. This is
    // the check that ties the two halves of the library together: if a builder
    // and the stub ever disagree about a magic word, one of them is wrong about
    // the manual.
    const std::vector<msel::Result<helpers::CanFrame>> commands = {
        msel::makeSetBaseAddressFrame(0x500u),
        msel::makeSetTransmitRateFrame(msel::TransmitRate::Hz100),
        msel::makeSetBaudAndShutdownDelayFrame(msel::CanBaud::Rate500k,
                                               std::chrono::milliseconds { 1500 }),
        msel::makeSetOutputDriveFrame(msel::OutputDrive::ActiveLowHighSide),
        msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled, 0x6E6u),
    };

    for (const auto& command : commands)
    {
        check(command.has_value(), "the command builds");
        if (!command)
        {
            continue;
        }
        const auto reply = relay.onFrame(*command);
        check(reply.has_value() &&
                  msel::decodeConfigResponse(reply->data_span()) == msel::ConfigResponse::Success,
              "the relay accepts a command built by this library");
    }

    // The base address command moved the relay, so its answer came back on the
    // new base rather than the old one.
    check(relay.addresses().base == 0x500u, "the relay moved to the new base address");
}

void test_can_kill_only_works_once_enabled()
{
    msel::StubRelay relay;
    const auto trigger = msel::makeCanKillTriggerFrame(msel::kDefaultKillAddress);
    check(trigger.has_value(), "the kill trigger builds");

    // Disabled from the factory, which is the state that matters: a stray frame
    // on 0x6E6 must not isolate the car.
    relay.onFrame(*trigger);
    check(!relay.isolated(), "a kill trigger does nothing while remote shutdown is disabled");

    relay.setExternalKillSwitchHeld(true);
    const auto enable = msel::makeSetCanShutdownFrame(msel::CanKillMode::Enabled,
                                                      msel::kDefaultKillAddress);
    relay.onFrame(*enable);
    relay.powerCycle();
    relay.setExternalKillSwitchHeld(false);

    check(relay.state().config.canKill == msel::CanKillMode::Enabled,
          "remote shutdown is enabled after the power cycle");

    // A frame on the right identifier but with the wrong payload is not a kill.
    relay.onFrame(frameOf(msel::kDefaultKillAddress, { 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
                                                       0x01 }));
    check(!relay.isolated(), "a near-miss payload does not isolate the car");

    relay.onFrame(*trigger);
    check(relay.isolated(), "the documented payload isolates the car");
    check(relay.state().statusRaw == static_cast<uint8_t>(msel::Status::CanTriggerKill),
          "and the relay reports a CAN trigger kill");
}

// Guards against the DBC and the hand-written configuration word drifting
// apart. They describe the same two bytes, and the decoder trusts the
// hand-written one, so a DBC edited to disagree would otherwise go unnoticed
// until someone read a setting off the generated code and got a different
// answer.
void test_dbc_agrees_with_the_hand_written_config_word()
{
    for (uint32_t raw = 0u; raw <= 0xFFFFu; ++raw)
    {
        const auto word = static_cast<uint16_t>(raw);
        const std::array<uint8_t, 8> bytes = {
            0x00u, 0x00u, 0x00u, 0x00u, static_cast<uint8_t>(word >> 8),
            static_cast<uint8_t>(word & 0xFFu), 0x00u, 0x00u
        };

        dbc_msel_master_relay::Master_Relay_Info_t msg;
        if (!msg.decode(bytes))
        {
            check(false, "the DBC failed to decode an eight-byte info frame");
            return;
        }

        const auto mine = msel::decodeConfigWord(word);
        const auto dbcCanKill = static_cast<uint8_t>(static_cast<int64_t>(msg.config_CAN_kill));
        const auto dbcBaud = static_cast<uint8_t>(static_cast<int64_t>(msg.config_CAN_baud));
        const auto dbcDrive = static_cast<uint8_t>(static_cast<int64_t>(msg.config_output_drive));
        const auto dbcDelay =
            std::chrono::milliseconds { std::llround(msg.config_shutdown_delay * 10.0) * 100 };

        if (dbcCanKill != mine.canKillRaw || dbcBaud != mine.baudRaw ||
            dbcDrive != mine.outputDriveRaw || dbcDelay != mine.shutdownDelay)
        {
            check(false, "the DBC and decodeConfigWord disagree about word 0x" +
                             std::to_string(raw));
            return;
        }
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_status_decodes();
    test_info_decodes();
    test_switch_state_decodes();
    test_base_address_rebasing();
    test_changing_the_base_address_keeps_the_callbacks();
    test_malformed_frames_are_rejected();
    test_config_response_and_status_share_an_identifier();
    test_stub_relay_round_trip();
    test_commands_need_the_external_kill_switch();
    test_relay_rejects_bad_commands();
    test_can_kill_only_works_once_enabled();
    test_dbc_agrees_with_the_hand_written_config_word();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all MSEL decode checks passed");
    return 0;
}
