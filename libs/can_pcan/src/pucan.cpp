// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_pcan/pucan.h"

#include "can/dlc.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstring>

namespace can::pcan
{
namespace
{

void put_u16(std::vector<uint8_t>& buffer, uint16_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& buffer, uint32_t value)
{
    put_u16(buffer, static_cast<uint16_t>(value & 0xFFFF));
    put_u16(buffer, static_cast<uint16_t>((value >> 16) & 0xFFFF));
}

uint16_t get_u16(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t get_u32(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// Records carry a 64-bit microsecond counter as two 32-bit halves.
uint64_t get_timestamp(std::span<const uint8_t> bytes, size_t offset)
{
    const uint64_t low = get_u32(bytes, offset);
    const uint64_t high = get_u32(bytes, offset + 4);
    return (high << 32) | low;
}

} // namespace

// ============================================================================
// Device identity
// ============================================================================

uint8_t channel_count_for_product(uint16_t productId)
{
    switch (productId)
    {
    case kProductUsbFd: return 1;
    case kProductUsbChip: return 1;
    case kProductUsbProFd: return 2;
    case kProductUsbX6: return 6;
    default: return 0;
    }
}

const char* product_name(uint16_t productId)
{
    switch (productId)
    {
    case kProductUsbFd: return "PCAN-USB FD";
    case kProductUsbChip: return "PCAN-Chip USB";
    case kProductUsbProFd: return "PCAN-USB Pro FD";
    case kProductUsbX6: return "PCAN-USB X6";
    default: return "unknown PEAK device";
    }
}

BitTimingLimits nominal_bit_timing_limits()
{
    BitTimingLimits limits;
    limits.clockHz = kControllerClockHz;
    limits.tseg1Min = 1;
    limits.tseg1Max = 64;
    limits.tseg2Min = 1;
    limits.tseg2Max = 16;
    limits.sjwMax = 16;
    limits.brpMin = 1;
    limits.brpMax = 1024;
    return limits;
}

BitTimingLimits data_bit_timing_limits()
{
    BitTimingLimits limits;
    limits.clockHz = kControllerClockHz;
    limits.tseg1Min = 1;
    limits.tseg1Max = 16;
    limits.tseg2Min = 1;
    limits.tseg2Max = 8;
    limits.sjwMax = 4;
    limits.brpMin = 1;
    limits.brpMax = 1024;
    return limits;
}

// ============================================================================
// Control requests
// ============================================================================

std::vector<uint8_t> encode_driver_loaded(bool loaded)
{
    std::vector<uint8_t> buffer(kFunctionDriverLoadedLength, 0);
    buffer[0] = 0;
    buffer[1] = loaded ? 1 : 0;
    return buffer;
}

std::string FirmwareInfo::firmwareVersionString() const
{
    return fmt::format("{}.{}.{}", firmwareVersion[0], firmwareVersion[1], firmwareVersion[2]);
}

Result<FirmwareInfo> decode_firmware_info(std::span<const uint8_t> bytes)
{
    if (bytes.size() < kFirmwareInfoMinLength)
    {
        return protocol_error(fmt::format(
            "firmware info is {} bytes; the record is at least {}", bytes.size(),
            kFirmwareInfoMinLength));
    }

    FirmwareInfo info;
    info.structureSize = get_u16(bytes, 0);
    info.recordType = get_u16(bytes, 2);
    info.hardwareType = bytes[4];
    info.bootloaderVersion[0] = bytes[5];
    info.bootloaderVersion[1] = bytes[6];
    info.bootloaderVersion[2] = bytes[7];
    info.hardwareVersion = bytes[8];
    info.firmwareVersion[0] = bytes[9];
    info.firmwareVersion[1] = bytes[10];
    info.firmwareVersion[2] = bytes[11];
    info.deviceId[0] = get_u32(bytes, 12);
    info.deviceId[1] = get_u32(bytes, 16);
    info.serialNumber = get_u32(bytes, 20);
    info.flags = get_u32(bytes, 24);

    // The extended record adds the endpoint numbers. Devices that report it
    // are telling us not to assume the two-channel layout -- the X6 does not
    // use it -- so the reported values win over the defaults.
    constexpr uint16_t kExtendedRecord = 2;
    if (info.recordType == kExtendedRecord && bytes.size() >= 33)
    {
        info.hasEndpoints = true;
        info.commandOutEndpoint = bytes[28];
        info.commandInEndpoint = bytes[29];
        info.dataOutEndpoint[0] = bytes[30];
        info.dataOutEndpoint[1] = bytes[31];
        info.dataInEndpoint = bytes[32];
    }

    return info;
}

// ============================================================================
// Commands
// ============================================================================

uint16_t opcode_channel(uint8_t channel, Opcode opcode)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(channel & 0x0F) << 12)
                                 | (static_cast<uint16_t>(opcode) & 0x03FF));
}

void append_command(std::vector<uint8_t>& buffer, uint8_t channel, Opcode opcode,
                    std::span<const uint8_t> args)
{
    put_u16(buffer, opcode_channel(channel, opcode));
    for (size_t i = 0; i < kCommandSize - 2; ++i)
    {
        buffer.push_back(i < args.size() ? args[i] : 0);
    }
}

void append_timing_slow(std::vector<uint8_t>& buffer, uint8_t channel, const BitTiming& timing,
                        uint8_t errorWarningLimit, bool tripleSampling)
{
    put_u16(buffer, opcode_channel(channel, Opcode::TimingSlow));
    buffer.push_back(errorWarningLimit);
    // EVERY field is counted from zero, not just the prescaler. A controller
    // given the un-decremented values runs one time quantum long in each
    // segment: 1 Mbit/s asked for as brp=2 tseg1=29 tseg2=10 comes out as
    // 952 kbit/s, which is 4.8% off and therefore cannot hold sync with
    // anything. The symptom is a bus that transmits nothing, receives nothing
    // and reports no error, because the controller never wins arbitration.
    //
    // The masks are the device's field widths: 7 bits for the jump width and
    // tseg2, 8 for tseg1, 10 for the prescaler. They were too narrow before,
    // which truncated a long tseg1 into a completely different bit rate at the
    // slower bus speeds.
    buffer.push_back(
        static_cast<uint8_t>(((timing.sjw - 1) & 0x7F) | (tripleSampling ? 0x80 : 0x00)));
    buffer.push_back(static_cast<uint8_t>((timing.tseg2 - 1) & 0x7F));
    buffer.push_back(static_cast<uint8_t>((timing.tseg1 - 1) & 0xFF));
    put_u16(buffer, static_cast<uint16_t>((timing.brp - 1) & 0x03FF));
}

void append_timing_fast(std::vector<uint8_t>& buffer, uint8_t channel, const BitTiming& timing)
{
    put_u16(buffer, opcode_channel(channel, Opcode::TimingFast));
    // No error warning limit in the data phase -- errors are counted in the
    // nominal phase -- so the byte is unused.
    buffer.push_back(0);
    // Counted from zero and masked to the data phase's own field widths --
    // 4 bits for the jump width and tseg2, 5 for tseg1 -- exactly as the
    // nominal phase above.
    buffer.push_back(static_cast<uint8_t>((timing.sjw - 1) & 0x0F));
    buffer.push_back(static_cast<uint8_t>((timing.tseg2 - 1) & 0x0F));
    buffer.push_back(static_cast<uint8_t>((timing.tseg1 - 1) & 0x1F));
    put_u16(buffer, static_cast<uint16_t>((timing.brp - 1) & 0x03FF));
}

void append_clock(std::vector<uint8_t>& buffer, uint8_t channel, uint8_t clockSelector)
{
    const uint8_t args[6] = { clockSelector, 0, 0, 0, 0, 0 };
    append_command(buffer, channel, Opcode::ClockSet, args);
}

void append_options(std::vector<uint8_t>& buffer, uint8_t channel, bool enable, uint16_t ucanMask,
                    uint16_t usbMask)
{
    put_u16(buffer, opcode_channel(channel, enable ? Opcode::SetEnableOption
                                                   : Opcode::ClearDisableOption));
    put_u16(buffer, ucanMask);
    put_u16(buffer, 0);
    put_u16(buffer, usbMask);
}

void append_std_filter_pass_all(std::vector<uint8_t>& buffer, uint8_t channel)
{
    // The 11-bit acceptance filter is a bitmap the device consults before
    // queueing a frame, and it is addressed a ROW AT A TIME: 64 rows of 32
    // identifiers each. "Pass everything" is therefore 64 commands, not one.
    //
    // Writing only row 0 -- which is what this did -- passes identifiers
    // 0x000..0x01F and silently drops the other 2016. Nothing reports it: the
    // device is working exactly as configured, and a bus whose traffic happens
    // to live above 0x01F simply looks dead.
    for (uint16_t row = 0; row < kStdFilterRowCount; ++row)
    {
        put_u16(buffer, opcode_channel(channel, Opcode::FilterStd));
        put_u16(buffer, row);
        put_u32(buffer, 0xFFFFFFFF);
    }
}

void append_led(std::vector<uint8_t>& buffer, uint8_t channel, uint8_t mode)
{
    const uint8_t args[6] = { mode, 0, 0, 0, 0, 0 };
    append_command(buffer, channel, Opcode::LedSet, args);
}

void finish_command_buffer(std::vector<uint8_t>& buffer, size_t bufferSize)
{
    // Eight 0xFF bytes, which is an end-of-collection opcode with every other
    // bit set too. Written as bytes rather than as a command so it is exactly
    // what the device is known to accept; a terminator built with channel 0
    // differs from the reference driver's in the channel nibble, and there is
    // no way to tell from here whether the firmware looks at it.
    //
    // Only fits if there is room. A buffer already full of commands is sent as
    // it is -- the device stops at the end of the transfer.
    if (buffer.size() + sizeof(uint64_t) <= bufferSize)
    {
        buffer.insert(buffer.end(), sizeof(uint64_t), 0xFF);
    }
    // Deliberately NOT padded out to bufferSize. The transfer carries the
    // commands and their terminator and nothing else, which is what the
    // reference driver sends; padding put 400-odd zero bytes on the wire that
    // the device had to read as no-op commands.
}

// ============================================================================
// Received records
// ============================================================================

DecodeResult decode_records(std::span<const uint8_t> buffer)
{
    DecodeResult result;
    size_t offset = 0;

    while (offset + 4 <= buffer.size())
    {
        const uint16_t size = get_u16(buffer, offset);
        const uint16_t rawType = get_u16(buffer, offset + 2);

        // A zero length is how the device pads the tail of a transfer: there
        // is nothing more in this buffer.
        if (size == 0)
        {
            break;
        }
        if (size < 4)
        {
            result.error = Error { Error::Kind::Protocol,
                                   fmt::format("record at offset {} claims {} bytes; the header "
                                               "alone is 4",
                                               offset, size),
                                   0 };
            break;
        }
        if (offset + size > buffer.size())
        {
            result.error = Error { Error::Kind::Protocol,
                                   fmt::format("record at offset {} claims {} bytes but only {} "
                                               "remain in the transfer",
                                               offset, size, buffer.size() - offset),
                                   0 };
            break;
        }

        const std::span<const uint8_t> record = buffer.subspan(offset, size);
        offset += size;

        Record decoded;
        decoded.type = static_cast<RecordType>(rawType);

        switch (decoded.type)
        {
        case RecordType::CanRx:
        case RecordType::CanTx:
        {
            // size, type, ts(8), tag(8), channel_dlc, client, flags(2), id(4)
            // is 28 bytes before the payload.
            constexpr size_t kFrameHeader = 28;
            if (record.size() < kFrameHeader)
            {
                result.error = Error { Error::Kind::Protocol,
                                       fmt::format("CAN record is {} bytes; the header is {}",
                                                   record.size(), kFrameHeader),
                                       0 };
                return result;
            }

            decoded.timestampUs = get_timestamp(record, 4);
            const uint8_t channelDlc = record[20];
            decoded.channel = static_cast<uint8_t>(channelDlc & 0x0F);
            const uint8_t dlc = static_cast<uint8_t>((channelDlc >> 4) & 0x0F);
            const uint16_t flags = get_u16(record, 22);
            const uint32_t id = get_u32(record, 24);

            helpers::CanFrame& frame = decoded.frame;
            frame.isFD = (flags & kFlagExtendedDataLength) != 0;
            frame.isExtended = (flags & kFlagExtendedId) != 0;
            frame.isRTR = (flags & kFlagRtr) != 0;
            frame.isBRS = (flags & kFlagBitrateSwitch) != 0;
            frame.isESI = (flags & kFlagErrorStateIndicator) != 0;
            frame.id = id & (frame.isExtended ? 0x1FFFFFFFu : 0x7FFu);
            frame.timestampUs = decoded.timestampUs;

            const uint8_t length = dlc_to_length(dlc, frame.isFD);
            const size_t available = record.size() - kFrameHeader;
            // An RTR frame has a length but carries no bytes, which is the one
            // case where the payload legitimately falls short of the DLC.
            const size_t copy = frame.isRTR ? 0 : std::min<size_t>(length, available);
            frame.len = static_cast<uint8_t>(frame.isRTR ? length : copy);
            for (size_t i = 0; i < copy; ++i)
            {
                frame.data[i] = record[kFrameHeader + i];
            }
            break;
        }

        case RecordType::Status:
        {
            constexpr size_t kStatusSize = 16;
            if (record.size() < kStatusSize)
            {
                result.error = Error { Error::Kind::Protocol,
                                       fmt::format("status record is {} bytes; expected {}",
                                                   record.size(), kStatusSize),
                                       0 };
                return result;
            }
            decoded.timestampUs = get_timestamp(record, 4);
            const uint8_t channelAndBits = record[12];
            decoded.channel = static_cast<uint8_t>(channelAndBits & 0x0F);

            // Most severe first: a bus-off controller is also error-passive
            // and warning, and reporting the mildest of those would be a lie.
            if ((channelAndBits & kStatusBusOff) != 0)
            {
                decoded.state = BusState::BusOff;
            }
            else if ((channelAndBits & kStatusErrorPassive) != 0)
            {
                decoded.state = BusState::ErrorPassive;
            }
            else if ((channelAndBits & kStatusErrorWarning) != 0)
            {
                decoded.state = BusState::ErrorWarning;
            }
            else
            {
                decoded.state = BusState::ErrorActive;
            }
            break;
        }

        case RecordType::Error:
        {
            constexpr size_t kErrorSize = 16;
            if (record.size() < kErrorSize)
            {
                result.error = Error { Error::Kind::Protocol,
                                       fmt::format("error record is {} bytes; expected {}",
                                                   record.size(), kErrorSize),
                                       0 };
                return result;
            }
            decoded.timestampUs = get_timestamp(record, 4);
            decoded.channel = static_cast<uint8_t>(record[12] & 0x0F);
            decoded.txErrorCounter = record[14];
            decoded.rxErrorCounter = record[15];
            break;
        }

        case RecordType::Overrun:
        {
            constexpr size_t kOverrunSize = 16;
            if (record.size() < kOverrunSize)
            {
                result.error = Error { Error::Kind::Protocol,
                                       fmt::format("overrun record is {} bytes; expected {}",
                                                   record.size(), kOverrunSize),
                                       0 };
                return result;
            }
            decoded.timestampUs = get_timestamp(record, 4);
            decoded.channel = static_cast<uint8_t>(record[12] & 0x0F);
            decoded.overrun = true;
            break;
        }

        case RecordType::Calibration:
        case RecordType::BusLoad:
        case RecordType::CacheCritical:
            // Recognised and carried through so a caller can count them, but
            // nothing here needs their contents.
            if (record.size() >= 12)
            {
                decoded.timestampUs = get_timestamp(record, 4);
            }
            break;
        }

        result.records.push_back(std::move(decoded));
    }

    return result;
}

// ============================================================================
// Transmitted frames
// ============================================================================

Result<void> append_tx_frame(std::vector<uint8_t>& buffer, uint8_t channel,
                             const helpers::CanFrame& frame, bool fdEnabled)
{
    if (!frame.id_fits())
    {
        return invalid_argument(fmt::format("identifier 0x{:X} does not fit an {}-bit frame",
                                            frame.id, frame.isExtended ? 29 : 11));
    }
    if (frame.isFD && !fdEnabled)
    {
        return invalid_argument(
            "the frame is CAN FD but this channel was opened without a data bit rate");
    }
    if (frame.isFD && frame.isRTR)
    {
        return invalid_argument("CAN FD has no remote transmission request");
    }

    const uint8_t maxLength = frame.isFD ? 64 : 8;
    if (frame.len > maxLength)
    {
        return invalid_argument(fmt::format("a {} frame carries at most {} bytes, not {}",
                                            frame.isFD ? "CAN FD" : "classic CAN", maxLength,
                                            frame.len));
    }
    if (!is_valid_can_length(frame.len, frame.isFD))
    {
        return invalid_argument(fmt::format(
            "{} bytes is not a length CAN FD can express; the next size up is {}", frame.len,
            round_up_can_length(frame.len, frame.isFD)));
    }

    const uint8_t dlc = length_to_dlc(frame.len, frame.isFD);
    const uint8_t onWire = frame.isRTR ? 0 : dlc_to_length(dlc, frame.isFD);

    uint16_t flags = 0;
    if (frame.isExtended) flags |= kFlagExtendedId;
    if (frame.isRTR) flags |= kFlagRtr;
    if (frame.isFD) flags |= kFlagExtendedDataLength;
    if (frame.isBRS) flags |= kFlagBitrateSwitch;
    if (frame.isESI) flags |= kFlagErrorStateIndicator;

    // size, type, tag(8), channel_dlc, client, flags(2), id(4) = 20 bytes,
    // then the payload. The whole record is padded to a multiple of four.
    constexpr size_t kTxHeader = 20;
    const size_t padded = (onWire + 3) & ~size_t { 3 };
    const uint16_t size = static_cast<uint16_t>(kTxHeader + padded);

    put_u16(buffer, size);
    put_u16(buffer, static_cast<uint16_t>(RecordType::CanTx));
    // The tag is echoed back on a looped-back frame. Nothing here uses it.
    put_u32(buffer, 0);
    put_u32(buffer, 0);
    buffer.push_back(static_cast<uint8_t>((channel & 0x0F) | (dlc << 4)));
    buffer.push_back(0); // client
    put_u16(buffer, flags);
    put_u32(buffer, frame.id);

    for (size_t i = 0; i < padded; ++i)
    {
        buffer.push_back(i < onWire ? frame.data[i] : 0);
    }

    return {};
}

} // namespace can::pcan
