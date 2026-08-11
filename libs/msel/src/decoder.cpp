// SPDX-License-Identifier: GPL-3.0-or-later

#include "msel/decoder.h"

#include <cmath>

#include "dbc_msel_master_relay.h"

namespace msel
{

namespace
{

namespace dbc = dbc_msel_master_relay;

// The DBC carries these as scaled doubles, but they are whole tenths on the
// wire. Recovering the tenth and multiplying up avoids handing on a duration of
// 999ms where the device said 1.0 seconds.
std::chrono::milliseconds tenthsToMilliseconds(double seconds)
{
    const auto tenths = std::llround(seconds * 10.0);
    return std::chrono::milliseconds { tenths * 100 };
}

uint8_t warningsMaskOf(const dbc::Master_Relay_Status_t& msg)
{
    uint8_t mask = 0u;
    const auto set = [&mask](bool bit, Warning warning) {
        if (bit)
        {
            mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(warning));
        }
    };

    set(msg.over_temp_warn != 0u, Warning::OverTemperature);
    set(msg.over_current_warn != 0u, Warning::OverCurrent);
    set(msg.low_voltage_warn != 0u, Warning::LowVoltage);
    set(msg.high_voltage_warn != 0u, Warning::HighVoltage);
    set(msg.over_temp_kill != 0u, Warning::OverTemperatureKill);
    set(msg.driver_kill != 0u, Warning::DriverSwitchKill);
    set(msg.external_kill != 0u, Warning::ExternalSwitchKill);
    set(msg.CAN_kill != 0u, Warning::CanTriggerKill);

    return mask;
}

} // namespace

Decoder::Decoder(Addresses addresses) : mAddresses(addresses) {}

Decoder::Accepted Decoder::onFrame(const helpers::CanFrame& frame)
{
    // The relay speaks classic CAN with 11-bit identifiers only. Anything else
    // sharing a numeric identifier is a different message from a different
    // device, and decoding it would be inventing data.
    if (frame.isExtended || frame.isRTR || frame.isError)
    {
        return Accepted::No;
    }

    const auto data = frame.data_span();

    // The watched identifier, when there is one and it is not already the base.
    // Responses only -- see watchConfigResponseAt. A frame here that is not a
    // response is somebody else's traffic, not this relay's telemetry, so it is
    // declined rather than falling through to the periodic decoders.
    if (mResponseWatch && frame.id == *mResponseWatch && frame.id != mAddresses.status())
    {
        if (const auto response = decodeConfigResponse(data))
        {
            mSnapshot.lastConfigResponse = response;
            if (mConfigResponseHandler)
            {
                mConfigResponseHandler(*response);
            }
            return Accepted::ConfigResponse;
        }
        return Accepted::No;
    }

    if (frame.id == mAddresses.status())
    {
        // Checked before the periodic decode, because both arrive here. See
        // decodeConfigResponse for why the two cannot be confused.
        if (const auto response = decodeConfigResponse(data))
        {
            mSnapshot.lastConfigResponse = response;
            if (mConfigResponseHandler)
            {
                mConfigResponseHandler(*response);
            }
            return Accepted::ConfigResponse;
        }

        dbc::Master_Relay_Status_t msg;
        if (!msg.decode(data))
        {
            return Accepted::No;
        }

        StatusFrame out;
        out.voltageOut = msg.voltage_out;
        out.loadCurrent = msg.load_current;
        out.temperatureInternal = msg.temperature_internal;
        out.warnings = warningsMaskOf(msg);
        out.statusRaw = static_cast<uint8_t>(static_cast<int64_t>(msg.status));
        out.status = statusFromRaw(out.statusRaw);

        mSnapshot.status = out;
        if (mStatusHandler)
        {
            mStatusHandler(out);
        }
        return Accepted::Status;
    }

    if (frame.id == mAddresses.info())
    {
        dbc::Master_Relay_Info_t msg;
        if (!msg.decode(data))
        {
            return Accepted::No;
        }

        InfoFrame out;
        out.voltageIn = msg.voltage_in;
        out.serialNo = static_cast<uint16_t>(msg.serial_no);
        out.timeSinceShutdown = tenthsToMilliseconds(msg.time_since_shutdown);
        out.shutdownCauseRaw = static_cast<uint8_t>(static_cast<int64_t>(msg.shutdown_cause));
        out.shutdownCause = statusFromRaw(out.shutdownCauseRaw);
        out.shutdownCause2Raw = static_cast<uint8_t>(static_cast<int64_t>(msg.shutdown_cause_2));
        out.shutdownCause2 = statusFromRaw(out.shutdownCause2Raw);

        // Taken from the raw bytes through decodeConfigWord rather than from
        // the DBC's three config signals, so that packing and unpacking the
        // configuration word is one piece of code with one set of tests. The
        // DBC still describes the same nibbles, and a test cross-checks the two
        // against each other so that editing one and not the other fails the
        // build rather than the car.
        const auto word = static_cast<uint16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);
        out.config = decodeConfigWord(word);

        mSnapshot.info = out;
        if (mInfoHandler)
        {
            mInfoHandler(out);
        }
        return Accepted::Info;
    }

    if (frame.id == mAddresses.switchState())
    {
        dbc::Master_Relay_Switch_State_t msg;
        if (!msg.decode(data))
        {
            return Accepted::No;
        }

        SwitchStateFrame out;
        out.switchStateRaw = static_cast<uint8_t>(msg.current_switch_state);
        out.switchState = switchStateFromRaw(out.switchStateRaw);

        mSnapshot.switchState = out;
        if (mSwitchStateHandler)
        {
            mSwitchStateHandler(out);
        }
        return Accepted::SwitchState;
    }

    return Accepted::No;
}

void Decoder::onStatus(std::function<void(const StatusFrame&)> handler)
{
    mStatusHandler = std::move(handler);
}

void Decoder::onInfo(std::function<void(const InfoFrame&)> handler)
{
    mInfoHandler = std::move(handler);
}

void Decoder::onSwitchState(std::function<void(const SwitchStateFrame&)> handler)
{
    mSwitchStateHandler = std::move(handler);
}

void Decoder::onConfigResponse(std::function<void(ConfigResponse)> handler)
{
    mConfigResponseHandler = std::move(handler);
}

} // namespace msel
