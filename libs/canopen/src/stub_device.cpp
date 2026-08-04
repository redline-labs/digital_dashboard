// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/stub_device.h"
#include "canopen/nmt.h"
#include "canopen/sdo.h"

#include <algorithm>

namespace canopen
{
namespace
{

constexpr uint8_t kCcsDownload = 0x20;
constexpr uint8_t kCcsUpload = 0x40;
constexpr uint8_t kCcsUploadSegment = 0x60;

constexpr uint8_t kScsUploadSegment = 0x00;
constexpr uint8_t kScsUpload = 0x40;
constexpr uint8_t kScsDownload = 0x60;
constexpr uint8_t kScsAbort = 0x80;

constexpr uint8_t kExpedited = 0x02;
constexpr uint8_t kSizeIndicated = 0x01;
constexpr uint8_t kToggle = 0x10;

// LSS command specifiers, from the same place the master takes them, so the
// two halves of the conversation cannot drift apart.
using namespace lss_cs;

uint8_t command_specifier(uint8_t byte)
{
    return byte & 0xE0;
}

uint64_t to_uint(std::span<const uint8_t> bytes)
{
    uint64_t value = 0;
    const size_t n = std::min<size_t>(bytes.size(), 8);
    for (size_t i = 0; i < n; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

std::vector<uint8_t> to_bytes(uint64_t value, size_t size)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return bytes;
}

// How wide the device serves an object. A fixed-width type serves its declared
// width; a string serves whatever it holds.
size_t width_for(DataType type, size_t fallback)
{
    if (auto bits = data_type_bits(type))
    {
        return (*bits + 7) / 8;
    }
    return fallback;
}

} // namespace

StubDevice::StubDevice(VirtualBus& bus, ObjectDictionary od, uint8_t nodeId, LssBitrate bitrate)
    : bus_(bus)
    , od_(std::move(od))
    , nodeId_(nodeId)
    , bitrate_(bitrate)
{
    seed_from_eds();
    bus_.attach([this](const helpers::CanFrame& frame) { handle(frame); });
}

void StubDevice::seed_from_eds()
{
    values_.clear();

    for (const auto& [index, object] : od_.objects)
    {
        for (const auto& [sub, declared] : object.subs)
        {
            Entry entry;
            entry.dataType = declared.dataType;
            entry.access = declared.access;
            entry.lowLimit = declared.lowLimit;
            entry.highLimit = declared.highLimit;

            const Key key { index, sub };
            auto stored = nonVolatile_.find(key);
            if (stored != nonVolatile_.end())
            {
                // Non-volatile memory wins over the factory default: that is
                // what "stored" means.
                entry.bytes = stored->second;
            }
            else if (declared.dataType == DataType::VisibleString
                     || declared.dataType == DataType::OctetString
                     || declared.dataType == DataType::UnicodeString)
            {
                std::string text;
                if (declared.defaultValue.has_value())
                {
                    if (const auto* value = std::get_if<std::string>(&*declared.defaultValue))
                    {
                        text = *value;
                    }
                }
                entry.bytes.assign(text.begin(), text.end());
            }
            else
            {
                // Resolved against the current node ID, which is how a real
                // device's COB-IDs follow an LSS node ID change.
                const uint64_t value = od_.defaultValue(index, sub, nodeId_).value_or(0);
                entry.bytes = to_bytes(value, width_for(declared.dataType, 4));
            }

            values_[key] = std::move(entry);
        }
    }
}

StubDevice::Entry* StubDevice::find(uint16_t index, uint8_t sub)
{
    auto it = values_.find(Key { index, sub });
    return it == values_.end() ? nullptr : &it->second;
}

const StubDevice::Entry* StubDevice::find(uint16_t index, uint8_t sub) const
{
    auto it = values_.find(Key { index, sub });
    return it == values_.end() ? nullptr : &it->second;
}

std::optional<uint64_t> StubDevice::value(uint16_t index, uint8_t sub) const
{
    const Entry* entry = find(index, sub);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    return to_uint(entry->bytes);
}

void StubDevice::set_value(uint16_t index, uint8_t sub, uint64_t value)
{
    Entry* entry = find(index, sub);
    if (entry != nullptr)
    {
        entry->bytes = to_bytes(value, entry->bytes.empty() ? 4 : entry->bytes.size());
    }
}

std::optional<uint64_t> StubDevice::stored_value(uint16_t index, uint8_t sub) const
{
    auto it = nonVolatile_.find(Key { index, sub });
    if (it == nonVolatile_.end())
    {
        return std::nullopt;
    }
    return to_uint(it->second);
}

void StubDevice::make_read_only(uint16_t index, uint8_t sub)
{
    Entry* entry = find(index, sub);
    if (entry != nullptr)
    {
        entry->forcedReadOnly = true;
    }
}

void StubDevice::reply(helpers::CanFrame frame)
{
    // Tagged with the rate the device is running at, so a client that has
    // moved the bus elsewhere does not hear it.
    bus_.inject(frame, responseDelay_, lss_bitrate_to_kbps(bitrate_));
}

void StubDevice::abort(uint16_t index, uint8_t sub, uint32_t code)
{
    helpers::CanFrame frame {};
    frame.id = SdoClient::kResponseCobIdBase + nodeId_;
    frame.len = 8;
    frame.data[0] = kScsAbort;
    frame.data[1] = static_cast<uint8_t>(index & 0xFF);
    frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    frame.data[3] = sub;
    for (size_t i = 0; i < 4; ++i)
    {
        frame.data[4 + i] = static_cast<uint8_t>((code >> (8 * i)) & 0xFF);
    }
    reply(frame);
}

void StubDevice::handle(const helpers::CanFrame& frame)
{
    if (!present_)
    {
        return;
    }

    // A device only hears a bus running at its own rate. This is the failure
    // that makes a mis-ordered reconfiguration unrecoverable, so it is modelled
    // rather than assumed away.
    if (bus_.bitrate_kbps() != lss_bitrate_to_kbps(bitrate_))
    {
        return;
    }

    const uint32_t id = frame.id & 0x7FF;

    if (id == NmtMaster::kNmtCobId)
    {
        handle_nmt(frame);
    }
    else if (id == LssMaster::kMasterCobId)
    {
        handle_lss(frame);
    }
    else if (id == SdoClient::kRequestCobIdBase + nodeId_)
    {
        handle_sdo(frame);
    }
}

void StubDevice::handle_nmt(const helpers::CanFrame& frame)
{
    if (frame.len < 2)
    {
        return;
    }
    // Byte 1 is the target; 0 addresses everyone.
    if (frame.data[1] != 0 && frame.data[1] != nodeId_)
    {
        return;
    }

    switch (static_cast<NmtCommand>(frame.data[0]))
    {
    case NmtCommand::Start:
        state_ = NmtState::Operational;
        break;
    case NmtCommand::Stop:
        state_ = NmtState::Stopped;
        break;
    case NmtCommand::EnterPreOperational:
        state_ = NmtState::PreOperational;
        break;
    case NmtCommand::ResetNode:
    case NmtCommand::ResetCommunication:
        reset();
        break;
    }
}

void StubDevice::reset()
{
    ++resetCount_;

    // LSS configuration is adopted here and nowhere else, which is why the
    // reconfiguration order matters: after this the device is at a different
    // address, possibly at a different bit rate, and everything the caller
    // knew about how to reach it is stale.
    if (storedNodeId_.has_value())
    {
        nodeId_ = *storedNodeId_;
    }
    if (storedBitrate_.has_value())
    {
        bitrate_ = *storedBitrate_;
    }

    // Values revert to their defaults except where a Store put them in
    // non-volatile memory.
    seed_from_eds();

    state_ = NmtState::PreOperational;
    lssConfiguration_ = false;
    segmented_ = Segmented {};

    // The boot-up frame: a heartbeat carrying state 0x00, emitted once. This
    // is what a client waits for instead of sleeping.
    //
    // Note that both its COB-ID and its bit rate are the ones the device has
    // *after* the reset. A client that changed either and did not follow will
    // not hear this, which is exactly the trap the reconfiguration ordering
    // exists to avoid.
    helpers::CanFrame bootup {};
    bootup.id = NmtMaster::kHeartbeatCobIdBase + nodeId_;
    bootup.len = 1;
    bootup.data[0] = 0x00;
    // Deliberately slower than an SDO answer: a reset really does take a while,
    // and a client that assumes otherwise should fail here.
    bus_.inject(bootup, Duration { 50 }, lss_bitrate_to_kbps(bitrate_));
}

void StubDevice::handle_sdo(const helpers::CanFrame& frame)
{
    if (frame.len != 8)
    {
        return;
    }

    const uint8_t command = frame.data[0];
    const uint16_t index = static_cast<uint16_t>(frame.data[1] | (frame.data[2] << 8));
    const uint8_t sub = frame.data[3];

    // --- segment of an upload in progress ---------------------------------
    if (command_specifier(command) == kCcsUploadSegment)
    {
        if (!segmented_.active)
        {
            abort(0, 0, static_cast<uint32_t>(SdoAbortCode::CommandSpecifierInvalid));
            return;
        }
        if (((command & kToggle) != 0) != segmented_.toggle)
        {
            segmented_ = Segmented {};
            abort(0, 0, static_cast<uint32_t>(SdoAbortCode::ToggleBitNotAlternated));
            return;
        }

        const size_t remaining = segmented_.bytes.size() - segmented_.offset;
        const size_t size = std::min<size_t>(remaining, 7);
        const bool last = size == remaining;

        helpers::CanFrame response {};
        response.id = SdoClient::kResponseCobIdBase + nodeId_;
        response.len = 8;
        response.data[0] = static_cast<uint8_t>(kScsUploadSegment
                                                | (segmented_.toggle ? kToggle : 0)
                                                | ((7 - size) << 1) | (last ? kSizeIndicated : 0));
        for (size_t i = 0; i < size; ++i)
        {
            response.data[1 + i] = segmented_.bytes[segmented_.offset + i];
        }
        segmented_.offset += size;
        segmented_.toggle = !segmented_.toggle;
        if (last)
        {
            segmented_ = Segmented {};
        }
        reply(response);
        return;
    }

    // --- does the object exist? -------------------------------------------
    const Object* object = od_.get(index);
    if (object == nullptr)
    {
        abort(index, sub, static_cast<uint32_t>(SdoAbortCode::ObjectDoesNotExist));
        return;
    }
    Entry* entry = find(index, sub);
    if (entry == nullptr)
    {
        abort(index, sub, static_cast<uint32_t>(SdoAbortCode::SubIndexDoesNotExist));
        return;
    }

    // --- upload ------------------------------------------------------------
    if (command_specifier(command) == kCcsUpload)
    {
        if (!is_readable(entry->access))
        {
            abort(index, sub, static_cast<uint32_t>(SdoAbortCode::ReadOfWriteOnly));
            return;
        }

        helpers::CanFrame response {};
        response.id = SdoClient::kResponseCobIdBase + nodeId_;
        response.len = 8;
        response.data[1] = static_cast<uint8_t>(index & 0xFF);
        response.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
        response.data[3] = sub;

        if (entry->bytes.size() <= 4)
        {
            // The command byte encodes the width, and this is the byte PDM
            // Manager compares for exact equality: 0x4F for one byte, 0x4B for
            // two, 0x47 for three, 0x43 for four.
            const size_t size = entry->bytes.size();
            response.data[0] = static_cast<uint8_t>(kScsUpload | kExpedited | kSizeIndicated
                                                    | ((4 - size) << 2));
            for (size_t i = 0; i < size; ++i)
            {
                response.data[4 + i] = entry->bytes[i];
            }
        }
        else
        {
            // Segmented: the first frame carries the total length.
            response.data[0] = kScsUpload | kSizeIndicated;
            const uint32_t total = static_cast<uint32_t>(entry->bytes.size());
            for (size_t i = 0; i < 4; ++i)
            {
                response.data[4 + i] = static_cast<uint8_t>((total >> (8 * i)) & 0xFF);
            }
            segmented_ = Segmented {};
            segmented_.active = true;
            segmented_.bytes = entry->bytes;
        }

        reply(response);
        return;
    }

    // --- download ----------------------------------------------------------
    if (command_specifier(command) == kCcsDownload)
    {
        if (entry->forcedReadOnly || !is_writable(entry->access))
        {
            abort(index, sub, static_cast<uint32_t>(SdoAbortCode::WriteOfReadOnly));
            return;
        }
        if ((command & kExpedited) == 0)
        {
            abort(index, sub, static_cast<uint32_t>(SdoAbortCode::UnsupportedAccess));
            return;
        }

        // Without the size indicator the client is saying "all four bytes".
        const size_t size = (command & kSizeIndicated) != 0 ? 4 - ((command >> 2) & 0x03) : 4;

        // The declared width is enforced. A device that quietly accepted a
        // one-byte write to a two-byte object would let the tool ship a bug
        // that only appears against real firmware.
        const size_t expected = width_for(entry->dataType, entry->bytes.size());
        if (size != expected)
        {
            abort(index, sub,
                  static_cast<uint32_t>(size > expected ? SdoAbortCode::LengthTooHigh
                                                        : SdoAbortCode::LengthTooLow));
            return;
        }

        std::vector<uint8_t> bytes(frame.data.begin() + 4, frame.data.begin() + 4 + size);
        const uint64_t incoming = to_uint(bytes);

        // Limits, where the file declares them. 0x2010:02's low limit of 0x40
        // is a real constraint on this device, and a tool that writes below it
        // should find out here.
        const int64_t asSigned = static_cast<int64_t>(incoming);
        if (entry->lowLimit.has_value() && asSigned < *entry->lowLimit)
        {
            abort(index, sub, static_cast<uint32_t>(SdoAbortCode::ValueTooLow));
            return;
        }
        if (entry->highLimit.has_value() && asSigned > *entry->highLimit)
        {
            abort(index, sub, static_cast<uint32_t>(SdoAbortCode::ValueTooHigh));
            return;
        }

        entry->bytes = std::move(bytes);

        // Store Parameters: the signature is checked, because a device that
        // saved on any write to 0x1010:01 would hide a tool sending the wrong
        // one.
        if (index == 0x1010 && incoming == 0x65766173) // "save", little-endian
        {
            for (const auto& [key, value] : values_)
            {
                nonVolatile_[key] = value.bytes;
            }
        }
        else if (index == 0x1011 && incoming == 0x64616F6C) // "load"
        {
            nonVolatile_.clear();
            storedNodeId_.reset();
            storedBitrate_.reset();
        }

        helpers::CanFrame response {};
        response.id = SdoClient::kResponseCobIdBase + nodeId_;
        response.len = 8;
        response.data[0] = kScsDownload;
        response.data[1] = static_cast<uint8_t>(index & 0xFF);
        response.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
        response.data[3] = sub;
        reply(response);
        return;
    }

    abort(index, sub, static_cast<uint32_t>(SdoAbortCode::CommandSpecifierInvalid));
}

void StubDevice::handle_lss(const helpers::CanFrame& frame)
{
    if (frame.len != 8)
    {
        return;
    }

    const uint8_t cs = frame.data[0];

    if (cs == kSwitchStateGlobal)
    {
        lssConfiguration_ = frame.data[1] == kModeConfiguration;
        return;
    }

    // Outside configuration mode every other command is ignored -- silently,
    // exactly as a real device does, which is why a tool that forgets to enter
    // configuration mode sees a timeout rather than an error.
    if (!lssConfiguration_)
    {
        return;
    }

    auto confirm = [&](uint8_t errorCode, uint8_t value)
    {
        helpers::CanFrame response {};
        response.id = LssMaster::kSlaveCobId;
        response.len = 8;
        response.data[0] = cs;
        response.data[1] = errorCode;
        response.data[2] = value;
        reply(response);
    };

    switch (cs)
    {
    case kConfigureNodeId:
    {
        const uint8_t requested = frame.data[1];
        if (requested < 1 || requested > 127)
        {
            confirm(1, 0);
            return;
        }
        pendingNodeId_ = requested;
        confirm(0, 0);
        return;
    }

    case kConfigureBitTiming:
    {
        if (frame.data[1] != 0)
        {
            // Only the CiA 301 standard table exists on this device.
            confirm(1, 0);
            return;
        }
        const uint8_t selector = frame.data[2];
        if (selector > static_cast<uint8_t>(LssBitrate::Rate10k))
        {
            confirm(1, 0);
            return;
        }
        const auto rate = static_cast<LssBitrate>(selector);
        // The EDS declares which rates the device supports; 10 kbit/s is
        // declared unsupported on this one and the device says so.
        auto declared = od_.deviceInfo.supportedBitrates.find(lss_bitrate_to_kbps(rate));
        if (declared != od_.deviceInfo.supportedBitrates.end() && !declared->second)
        {
            confirm(1, 0);
            return;
        }
        pendingBitrate_ = rate;
        confirm(0, 0);
        return;
    }

    case kStoreConfiguration:
        // Node ID and bit rate persist through LSS Store, which is a different
        // mechanism from the SDO 0x1010 "save" that persists everything else.
        if (pendingNodeId_.has_value())
        {
            storedNodeId_ = pendingNodeId_;
        }
        if (pendingBitrate_.has_value())
        {
            storedBitrate_ = pendingBitrate_;
        }
        confirm(0, 0);
        return;

    case kInquireNodeId:
        confirm(nodeId_, 0);
        return;

    case kActivateBitTiming:
        if (pendingBitrate_.has_value())
        {
            bitrate_ = *pendingBitrate_;
        }
        return;

    default:
        return;
    }
}

void StubDevice::emit_tpdo1(std::span<const uint8_t> payload)
{
    const uint64_t cobid = value(0x1800, 1).value_or(0);

    helpers::CanFrame frame {};
    frame.id = static_cast<uint32_t>(cobid & 0x7FF);
    frame.len = static_cast<uint8_t>(std::min<size_t>(payload.size(), frame.data.size()));
    for (uint8_t i = 0; i < frame.len; ++i)
    {
        frame.data[i] = payload[i];
    }
    bus_.inject(frame, Duration { 0 }, lss_bitrate_to_kbps(bitrate_));
}

} // namespace canopen
