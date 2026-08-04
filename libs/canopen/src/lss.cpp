// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/lss.h"

#include <spdlog/fmt/fmt.h>

namespace canopen
{
namespace
{

// Command specifiers, from the Grayhill manual sections 9.1 and 9.3, which
// agree with CiA DS-305. Named in the header so a dry run can describe them.
using namespace lss_cs;

LssError timeout_error(uint8_t cs, Duration timeout)
{
    return LssError { LssError::Kind::Timeout, 0, 0,
                      fmt::format("no LSS response to command 0x{:02X} within {} ms", cs,
                                  timeout.count()) };
}

} // namespace

std::optional<LssBitrate> lss_bitrate_from_kbps(uint32_t kbps)
{
    switch (kbps)
    {
    case 1000: return LssBitrate::Rate1000k;
    case 800: return LssBitrate::Rate800k;
    case 500: return LssBitrate::Rate500k;
    case 250: return LssBitrate::Rate250k;
    case 125: return LssBitrate::Rate125k;
    case 100: return LssBitrate::Rate100k;
    case 50: return LssBitrate::Rate50k;
    case 20: return LssBitrate::Rate20k;
    case 10: return LssBitrate::Rate10k;
    default: return std::nullopt;
    }
}

uint32_t lss_bitrate_to_kbps(LssBitrate rate)
{
    switch (rate)
    {
    case LssBitrate::Rate1000k: return 1000;
    case LssBitrate::Rate800k: return 800;
    case LssBitrate::Rate500k: return 500;
    case LssBitrate::Rate250k: return 250;
    case LssBitrate::Rate125k: return 125;
    case LssBitrate::Rate100k: return 100;
    case LssBitrate::Rate50k: return 50;
    case LssBitrate::Rate20k: return 20;
    case LssBitrate::Rate10k: return 10;
    }
    return 0;
}

const char* to_string(LssBitrate rate)
{
    switch (rate)
    {
    case LssBitrate::Rate1000k: return "1 Mbit/s";
    case LssBitrate::Rate800k: return "800 kbit/s";
    case LssBitrate::Rate500k: return "500 kbit/s";
    case LssBitrate::Rate250k: return "250 kbit/s";
    case LssBitrate::Rate125k: return "125 kbit/s";
    case LssBitrate::Rate100k: return "100 kbit/s";
    case LssBitrate::Rate50k: return "50 kbit/s";
    case LssBitrate::Rate20k: return "20 kbit/s";
    case LssBitrate::Rate10k: return "10 kbit/s";
    }
    return "unknown";
}

std::string to_string(const LssError& error)
{
    return error.message;
}

helpers::CanFrame make_lss_frame(uint8_t commandSpecifier, uint8_t arg0, uint8_t arg1)
{
    helpers::CanFrame frame {};
    frame.id = LssMaster::kMasterCobId;
    frame.len = 8;
    frame.data[0] = commandSpecifier;
    frame.data[1] = arg0;
    frame.data[2] = arg1;
    return frame;
}

LssMaster::LssMaster(Bus& bus, bool singleNodeBus, Duration timeout)
    : bus_(bus)
    , singleNodeBus_(singleNodeBus)
    , timeout_(timeout)
{
    bus_.subscribe(
        [this](const helpers::CanFrame& frame)
        {
            if ((frame.id & 0x7FF) == kSlaveCobId)
            {
                pending_ = frame;
                pendingPresent_ = true;
            }
        });
}

void LssMaster::log(const std::string& line) const
{
    if (exchangeCallback_)
    {
        exchangeCallback_(line);
    }
}

LssResult<void> LssMaster::require_single_node_bus(const char* what) const
{
    if (singleNodeBus_)
    {
        return {};
    }
    return std::unexpected(LssError {
        LssError::Kind::Refused, 0, 0,
        fmt::format("{} broadcasts to every LSS device on the bus; refusing without an "
                    "assertion that the keypad is the only one on it",
                    what) });
}

LssResult<void> LssMaster::send_unconfirmed(uint8_t cs, uint8_t arg0, uint8_t arg1)
{
    log(fmt::format("-> LSS 0x{:02X} {:02X} {:02X} (unconfirmed)", cs, arg0, arg1));
    bus_.send(make_lss_frame(cs, arg0, arg1));
    return {};
}

LssResult<helpers::CanFrame> LssMaster::send_confirmed(uint8_t cs, uint8_t arg0, uint8_t arg1)
{
    pendingPresent_ = false;

    log(fmt::format("-> LSS 0x{:02X} {:02X} {:02X}", cs, arg0, arg1));
    bus_.send(make_lss_frame(cs, arg0, arg1));

    if (!wait_until(bus_, timeout_, [this] { return pendingPresent_; }))
    {
        return std::unexpected(timeout_error(cs, timeout_));
    }

    const helpers::CanFrame response = pending_;
    pendingPresent_ = false;
    log(fmt::format("<- LSS 0x{:02X} {:02X} {:02X}", response.data[0], response.data[1],
                    response.data[2]));

    if (response.len != 8)
    {
        return std::unexpected(LssError {
            LssError::Kind::BadResponse, 0, 0,
            fmt::format("LSS response to 0x{:02X} has {} byte(s), expected 8", cs, response.len) });
    }

    if (response.data[0] != cs)
    {
        return std::unexpected(LssError {
            LssError::Kind::BadResponse, 0, 0,
            fmt::format("sent LSS command 0x{:02X} but the answer was to 0x{:02X}", cs,
                        response.data[0]) });
    }

    // Byte 1 is the error code and byte 2 is a specific error that is only
    // meaningful when byte 1 is 0xFF. Both are reported so a device that says
    // "implementation specific" does not lose the detail.
    if (response.data[1] != 0)
    {
        return std::unexpected(LssError {
            LssError::Kind::Rejected, response.data[1], response.data[2],
            fmt::format("node rejected LSS command 0x{:02X}: error code {}, specific error {}", cs,
                        response.data[1], response.data[2]) });
    }

    return response;
}

LssResult<void> LssMaster::enter_configuration()
{
    if (auto refusal = require_single_node_bus("LSS switch state global"); !refusal.has_value())
    {
        return refusal;
    }
    return send_unconfirmed(kSwitchStateGlobal, kModeConfiguration);
}

LssResult<void> LssMaster::exit_configuration()
{
    if (auto refusal = require_single_node_bus("LSS switch state global"); !refusal.has_value())
    {
        return refusal;
    }
    return send_unconfirmed(kSwitchStateGlobal, kModeWaiting);
}

LssResult<void> LssMaster::configure_node_id(uint8_t nodeId)
{
    // CiA 301 reserves 0 for broadcast and stops at 127. A device asked for
    // one of the others answers with an error code, but there is no reason to
    // find that out over the wire.
    if (nodeId < 1 || nodeId > 127)
    {
        return std::unexpected(LssError {
            LssError::Kind::Refused, 0, 0,
            fmt::format("node ID {} is outside the valid range 1..127", nodeId) });
    }

    auto response = send_confirmed(kConfigureNodeId, nodeId);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return {};
}

LssResult<void> LssMaster::configure_bitrate(LssBitrate rate)
{
    auto response
        = send_confirmed(kConfigureBitTiming, kStandardBitTimingTable, static_cast<uint8_t>(rate));
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return {};
}

LssResult<void> LssMaster::store_configuration()
{
    auto response = send_confirmed(kStoreConfiguration, 0);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return {};
}

LssResult<void> LssMaster::activate_bitrate(Duration delay)
{
    if (auto refusal = require_single_node_bus("LSS activate bit timing"); !refusal.has_value())
    {
        return refusal;
    }

    const uint16_t ms = static_cast<uint16_t>(delay.count());
    log(fmt::format("-> LSS activate bit timing after {} ms (unconfirmed)", ms));
    bus_.send(make_lss_frame(kActivateBitTiming, static_cast<uint8_t>(ms & 0xFF),
                             static_cast<uint8_t>((ms >> 8) & 0xFF)));
    return {};
}

LssResult<uint8_t> LssMaster::inquire_node_id()
{
    auto response = send_confirmed(kInquireNodeId, 0);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    // An inquire response carries the value in byte 1 rather than an error
    // code, so send_confirmed's "byte 1 is an error" rule does not apply and a
    // non-zero node ID would have been reported as a rejection. Re-read it
    // from the raw frame instead.
    return response->data[1];
}

void LssMaster::on_exchange(std::function<void(const std::string&)> callback)
{
    exchangeCallback_ = std::move(callback);
}

} // namespace canopen
