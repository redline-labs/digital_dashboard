// SPDX-License-Identifier: GPL-3.0-or-later

#include "msel/stub_relay.h"

#include <array>
#include <cmath>

namespace msel
{

namespace
{

constexpr size_t kCommandLength = 8u;

void putU16(helpers::CanFrame& frame, size_t offset, uint16_t value)
{
    frame.data[offset] = static_cast<uint8_t>(value >> 8);
    frame.data[offset + 1u] = static_cast<uint8_t>(value & 0xFFu);
}

void putI16(helpers::CanFrame& frame, size_t offset, int16_t value)
{
    putU16(frame, offset, static_cast<uint16_t>(value));
}

int16_t scaledI16(double value, double scale)
{
    const auto raw = std::llround(value / scale);
    if (raw > 32767)
    {
        return 32767;
    }
    if (raw < -32768)
    {
        return -32768;
    }
    return static_cast<int16_t>(raw);
}

uint16_t scaledU16(double value, double scale)
{
    const auto raw = std::llround(value / scale);
    if (raw > 65535)
    {
        return 65535;
    }
    if (raw < 0)
    {
        return 0;
    }
    return static_cast<uint16_t>(raw);
}

uint16_t wordAt(const helpers::CanFrame& frame, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(frame.data[offset]) << 8) |
                                 frame.data[offset + 1u]);
}

} // namespace

StubRelay::StubRelay(Addresses addresses) : mAddresses(addresses)
{
    // Factory defaults, per the manual: 1Mbps, remote shutdown off, active high
    // half bridge, one second of hold-up.
    mState.config.canKillRaw = static_cast<uint8_t>(CanKillMode::Disabled);
    mState.config.canKill = CanKillMode::Disabled;
    mState.config.baudRaw = static_cast<uint8_t>(CanBaud::Rate1M);
    mState.config.baud = CanBaud::Rate1M;
    mState.config.outputDriveRaw = static_cast<uint8_t>(OutputDrive::ActiveHighHalfBridge);
    mState.config.outputDrive = OutputDrive::ActiveHighHalfBridge;
    mState.config.shutdownDelay = std::chrono::milliseconds { 1000 };
}

std::vector<helpers::CanFrame> StubRelay::periodicFrames() const
{
    std::vector<helpers::CanFrame> frames;

    // Manual table 5. Packed here from the byte table rather than through the
    // DBC, on purpose -- see the header.
    helpers::CanFrame status;
    status.id = mAddresses.status();
    status.len = 8u;
    putU16(status, 0u, scaledU16(mState.voltageOut, 0.01));
    putI16(status, 2u, scaledI16(mState.loadCurrent, 0.1));
    putI16(status, 4u, scaledI16(mState.temperatureInternal, 0.1));
    status.data[6] = mState.warnings;
    status.data[7] = mState.statusRaw;
    frames.push_back(status);

    // Manual table 6.
    helpers::CanFrame info;
    info.id = mAddresses.info();
    info.len = 8u;
    putU16(info, 0u, scaledU16(mState.voltageIn, 0.01));
    putU16(info, 2u, mState.serialNo);
    putU16(info, 4u, encodeConfigWord(mState.config));
    info.data[6] = static_cast<uint8_t>(mState.timeSinceShutdown.count() / 100);
    // Byte 7 packs the two causes into one byte: the previous event in the high
    // nibble, the most recent in the low one.
    info.data[7] = static_cast<uint8_t>(((mState.shutdownCause2Raw & 0x0Fu) << 4) |
                                        (mState.shutdownCauseRaw & 0x0Fu));
    frames.push_back(info);

    // Manual table 7. Two bytes, and only on firmware new enough to have it.
    if (mState.transmitsSwitchState)
    {
        helpers::CanFrame switchState;
        switchState.id = mAddresses.switchState();
        switchState.len = 2u;
        switchState.data[0] = 0u;
        switchState.data[1] = mState.switchStateRaw;
        frames.push_back(switchState);
    }

    return frames;
}

helpers::CanFrame StubRelay::makeResponse(ConfigResponse response) const
{
    // Every response is the same byte eight times, sent on the base address --
    // the same identifier the periodic status message uses.
    helpers::CanFrame frame;
    frame.id = mAddresses.status();
    frame.len = static_cast<uint8_t>(kCommandLength);
    for (size_t i = 0u; i < kCommandLength; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(response);
    }
    return frame;
}

std::optional<helpers::CanFrame> StubRelay::onFrame(const helpers::CanFrame& frame)
{
    if (frame.id == mKillAddress && mState.config.canKill.has_value() &&
        *mState.config.canKill != CanKillMode::Disabled)
    {
        const std::array<uint8_t, kCommandLength> expected = { 0xFFu, 0x00u, 0xFFu, 0x00u,
                                                               0xFFu, 0x00u, 0xFFu, 0x00u };
        if (frame.len == kCommandLength)
        {
            bool matches = true;
            for (size_t i = 0u; i < kCommandLength; ++i)
            {
                if (frame.data[i] != expected[i])
                {
                    matches = false;
                }
            }
            if (matches)
            {
                mIsolated = true;
                mState.statusRaw = static_cast<uint8_t>(Status::CanTriggerKill);
                mState.warnings = static_cast<uint8_t>(Warning::CanTriggerKill);
                mState.shutdownCause2Raw = mState.shutdownCauseRaw;
                mState.shutdownCauseRaw = static_cast<uint8_t>(Status::CanTriggerKill);
                return std::nullopt;
            }
        }
    }

    if (frame.id == kConfigCommandId)
    {
        return handleCommand(frame);
    }

    return std::nullopt;
}

std::optional<helpers::CanFrame> StubRelay::handleCommand(const helpers::CanFrame& frame)
{
    // No held switch, no effect and no answer. This is the single most
    // surprising thing about configuring this device, so the stub enforces it
    // rather than letting a test pass that the car would not.
    if (!mExternalKillHeld)
    {
        return std::nullopt;
    }

    if (frame.len != kCommandLength)
    {
        return makeResponse(ConfigResponse::FrameCheckError);
    }

    const uint16_t magic = wordAt(frame, 0u);
    const uint16_t trailer = wordAt(frame, 6u);

    // The magic is repeated at both ends; a payload whose ends disagree is not
    // a command this device will act on.
    if (magic != trailer)
    {
        return makeResponse(ConfigResponse::FrameCheckError);
    }

    // How the four documented response codes map onto what can be wrong is not
    // spelled out by the manual; this is the reading the codes' names support.
    // "Frame check error" covers a malformed envelope, "IDs do not match"
    // covers the two copies of a value disagreeing, and "invalid ID" covers a
    // well-formed value the device will not accept.
    const uint16_t first = wordAt(frame, 2u);
    const uint16_t second = wordAt(frame, 4u);

    switch (magic)
    {
    case 0x0789u:
    {
        if (first != second)
        {
            return makeResponse(ConfigResponse::IdMismatch);
        }
        if (!validateBaseAddress(first))
        {
            return makeResponse(ConfigResponse::InvalidId);
        }
        // The base address is the one setting that moves without a restart.
        mAddresses.base = first;
        return makeResponse(ConfigResponse::Success);
    }

    case 0x0DEFu:
    {
        if (frame.data[2] != frame.data[3])
        {
            return makeResponse(ConfigResponse::IdMismatch);
        }
        if (!transmitRateFromRaw(frame.data[2]))
        {
            return makeResponse(ConfigResponse::InvalidId);
        }
        // Nothing in the reported state changes: the transmit rate is not read
        // back in any message.
        return makeResponse(ConfigResponse::Success);
    }

    case 0x0456u:
    {
        if (frame.data[2] != frame.data[3] || frame.data[4] != frame.data[5])
        {
            return makeResponse(ConfigResponse::IdMismatch);
        }
        const auto baud = canBaudFromRaw(frame.data[2]);
        if (!baud)
        {
            return makeResponse(ConfigResponse::InvalidId);
        }

        // The shutdown delay is live immediately; the baud rate waits for a
        // power cycle. Splitting them is the whole reason this command is
        // reported as needing a restart.
        mState.config.shutdownDelay = std::chrono::milliseconds { frame.data[4] * 100 };

        Config pending = mPendingConfig.value_or(mState.config);
        pending.shutdownDelay = mState.config.shutdownDelay;
        pending.baudRaw = frame.data[2];
        pending.baud = baud;
        mPendingConfig = pending;
        return makeResponse(ConfigResponse::Success);
    }

    case 0x0ABCu:
    {
        if (frame.data[2] != frame.data[3])
        {
            return makeResponse(ConfigResponse::IdMismatch);
        }
        const auto drive = outputDriveFromRaw(frame.data[2]);
        if (!drive)
        {
            return makeResponse(ConfigResponse::InvalidId);
        }

        Config pending = mPendingConfig.value_or(mState.config);
        pending.outputDriveRaw = frame.data[2];
        pending.outputDrive = drive;
        mPendingConfig = pending;
        return makeResponse(ConfigResponse::Success);
    }

    case 0x0123u:
    {
        if (first != second)
        {
            return makeResponse(ConfigResponse::IdMismatch);
        }
        const auto mode = canKillModeFromRaw(static_cast<uint8_t>((first >> 12) & 0x0Fu));
        const auto address = static_cast<uint32_t>(first & 0x0FFFu);
        if (!mode || address > kMaxStandardId)
        {
            return makeResponse(ConfigResponse::InvalidId);
        }

        Config pending = mPendingConfig.value_or(mState.config);
        pending.canKillRaw = static_cast<uint8_t>(*mode);
        pending.canKill = mode;
        mPendingConfig = pending;
        mPendingKillAddress = address;
        return makeResponse(ConfigResponse::Success);
    }

    default:
        return makeResponse(ConfigResponse::FrameCheckError);
    }
}

void StubRelay::powerCycle()
{
    if (mPendingConfig)
    {
        mState.config = *mPendingConfig;
        mPendingConfig.reset();
    }
    if (mPendingKillAddress)
    {
        mKillAddress = *mPendingKillAddress;
        mPendingKillAddress.reset();
    }

    mIsolated = false;
    mState.statusRaw = static_cast<uint8_t>(Status::Normal);
    mState.warnings = 0u;
}

} // namespace msel
