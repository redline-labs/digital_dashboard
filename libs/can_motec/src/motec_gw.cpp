// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_motec/motec_gw.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstring>

namespace can::motec
{
namespace
{

void put_be16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void put_be32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint16_t get_be16(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

uint32_t get_be32(std::span<const uint8_t> bytes, size_t offset)
{
    return (static_cast<uint32_t>(bytes[offset]) << 24)
        | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 8) | static_cast<uint32_t>(bytes[offset + 3]);
}

bool has_preamble(std::span<const uint8_t> bytes, size_t offset)
{
    return offset + 3 <= bytes.size() && bytes[offset] == kPreamble0
        && bytes[offset + 1] == kPreamble1 && bytes[offset + 2] == kPreamble2;
}

} // namespace

const char* to_string(Code code)
{
    switch (code)
    {
    case Code::Open: return "Open";
    case Code::Poll: return "Poll";
    case Code::Ack: return "Ack";
    case Code::RegRead: return "RegRead";
    case Code::Filter: return "Filter";
    case Code::Tx: return "Tx";
    case Code::Rx: return "Rx";
    case Code::Set: return "Set";
    case Code::Version: return "Version";
    }
    return "unknown";
}

uint16_t crc16_ccitt(std::span<const uint8_t> bytes)
{
    uint16_t crc = 0xFFFF;
    for (const uint8_t byte : bytes)
    {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(byte) << 8);
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x8000u) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                       : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::optional<uint8_t> Frame::status() const
{
    if (payload.empty())
    {
        return std::nullopt;
    }
    return payload[0];
}

std::vector<uint8_t> encode_frame(const Frame& frame)
{
    // The CRC-covered region is built on its own rather than being carved out
    // of the finished datagram with an offset. It is the protocol's own unit --
    // the length byte counts these bytes and the CRC covers exactly them -- so
    // naming it here means the "3" that skips the preamble appears once, at
    // the point where the preamble is prepended, instead of in the length, the
    // CRC range and the reader.
    std::vector<uint8_t> covered;
    covered.push_back(static_cast<uint8_t>(4 + frame.payload.size()));
    covered.push_back(frame.cmd);
    covered.push_back(frame.field5);
    covered.push_back(frame.reqid);
    covered.insert(covered.end(), frame.payload.begin(), frame.payload.end());

    const uint16_t crc = crc16_ccitt(covered);

    std::vector<uint8_t> out;
    out.push_back(kPreamble0);
    out.push_back(kPreamble1);
    out.push_back(kPreamble2);
    out.insert(out.end(), covered.begin(), covered.end());
    put_be16(out, crc);
    out.insert(out.end(), frame.data.begin(), frame.data.end());
    return out;
}

size_t data_block_length(const Frame& frame)
{
    switch (frame.code())
    {
    case Code::Tx:
        // Only the request carries records. The response's BE16 is the count
        // of bytes accepted, with nothing behind it.
        if (frame.isReply() || frame.payload.size() < 2)
        {
            return 0;
        }
        return get_be16(frame.payload, 0);

    case Code::Rx:
    case Code::RegRead:
        // Only the response carries a block: [status][BE16 length].
        if (!frame.isReply() || frame.payload.size() < 3)
        {
            return 0;
        }
        return get_be16(frame.payload, 1);

    case Code::Open:
    case Code::Poll:
    case Code::Ack:
    case Code::Filter:
    case Code::Set:
    case Code::Version:
        return 0;
    }
    return 0;
}

Result<Frame> decode_frame(std::span<const uint8_t> bytes)
{
    if (bytes.size() < kMinFrameSize)
    {
        return protocol_error(fmt::format("a gateway frame is at least {} bytes, got {}",
                                          kMinFrameSize, bytes.size()));
    }
    if (!has_preamble(bytes, 0))
    {
        return protocol_error(fmt::format("expected the preamble 80 81 86, got {:02X} {:02X} {:02X}",
                                          bytes[0], bytes[1], bytes[2]));
    }

    const size_t covered = bytes[3];
    if (covered < kMinCoveredLength)
    {
        return protocol_error(fmt::format(
            "a frame's covered length is at least {}, got {}", kMinCoveredLength, covered));
    }
    if (bytes.size() < 3 + covered + 2)
    {
        return protocol_error(fmt::format("a frame declaring {} covered bytes needs {}, got {}",
                                          covered, 3 + covered + 2, bytes.size()));
    }

    const uint16_t expected = crc16_ccitt(bytes.subspan(3, covered));
    const uint16_t actual = get_be16(bytes, 3 + covered);
    if (expected != actual)
    {
        return protocol_error(
            fmt::format("frame CRC is 0x{:04X}, expected 0x{:04X}", actual, expected));
    }

    Frame frame;
    frame.cmd = bytes[4];
    frame.field5 = bytes[5];
    frame.reqid = bytes[6];
    frame.payload.assign(bytes.begin() + 7, bytes.begin() + 3 + static_cast<ptrdiff_t>(covered));

    const size_t dataLength = data_block_length(frame);
    if (dataLength != 0)
    {
        const size_t start = 3 + covered + 2;
        if (bytes.size() < start + dataLength)
        {
            return protocol_error(
                fmt::format("a {} frame declares a {}-byte data block but only {} byte(s) follow",
                            to_string(frame.code()), dataLength, bytes.size() - start));
        }
        frame.data.assign(bytes.begin() + static_cast<ptrdiff_t>(start),
                          bytes.begin() + static_cast<ptrdiff_t>(start + dataLength));
    }

    return frame;
}

// --- records ---------------------------------------------------------------

void pack_record(std::span<uint8_t, kRecordSize> out, const Record& record)
{
    out[0] = static_cast<uint8_t>(record.wireId >> 24);
    out[1] = static_cast<uint8_t>((record.wireId >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((record.wireId >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(record.wireId & 0xFF);
    out[4] = record.flags;
    std::memcpy(out.data() + 5, record.data.data(), record.data.size());
    out[13] = static_cast<uint8_t>(record.timestampUs >> 24);
    out[14] = static_cast<uint8_t>((record.timestampUs >> 16) & 0xFF);
    out[15] = static_cast<uint8_t>((record.timestampUs >> 8) & 0xFF);
    out[16] = static_cast<uint8_t>(record.timestampUs & 0xFF);
}

Record unpack_record(std::span<const uint8_t, kRecordSize> in)
{
    Record record;
    record.wireId = get_be32(in, 0);
    record.flags = in[4];
    std::memcpy(record.data.data(), in.data() + 5, record.data.size());
    record.timestampUs = get_be32(in, 13);
    return record;
}

std::vector<Record> unpack_records(std::span<const uint8_t> block)
{
    std::vector<Record> records;
    records.reserve(block.size() / kRecordSize);
    for (size_t offset = 0; offset + kRecordSize <= block.size(); offset += kRecordSize)
    {
        records.push_back(unpack_record(block.subspan(offset).first<kRecordSize>()));
    }
    return records;
}

std::vector<uint8_t> pack_records(std::span<const Record> records)
{
    std::vector<uint8_t> block(records.size() * kRecordSize);
    for (size_t i = 0; i < records.size(); ++i)
    {
        pack_record(std::span(block).subspan(i * kRecordSize).first<kRecordSize>(), records[i]);
    }
    return block;
}

helpers::CanFrame to_can_frame(const Record& record)
{
    helpers::CanFrame frame;
    frame.id = record.busId();
    frame.isExtended = record.extended();
    // The device has no FD support and reports no error frames, so these stay
    // false rather than being guessed at from the undocumented flag bits.
    frame.isFD = false;
    frame.isBRS = false;
    frame.isESI = false;
    frame.isError = false;
    frame.isRTR = false;

    // Clamped because the DLC nibble can hold 9..15, which classic CAN does
    // not use but nothing stops a damaged record from carrying.
    frame.len = std::min<uint8_t>(record.dlc(), 8);
    std::copy_n(record.data.begin(), frame.len, frame.data.begin());

    frame.timestampUs = record.timestampUs;
    return frame;
}

Result<Record> from_can_frame(const helpers::CanFrame& frame)
{
    if (frame.isFD)
    {
        return unsupported("a MoTeC UTC is a classic CAN device and cannot send an FD frame");
    }
    if (frame.len > 8)
    {
        return invalid_argument(
            fmt::format("classic CAN carries at most 8 bytes, this frame has {}", frame.len));
    }
    if (!frame.id_fits())
    {
        return invalid_argument(fmt::format("identifier 0x{:X} does not fit a {}-bit frame",
                                            frame.id, frame.isExtended ? 29 : 11));
    }
    if (frame.isRTR)
    {
        // The flags byte's upper bits are not understood, and one of them is
        // presumably RTR. Refusing beats setting a bit on a guess and putting
        // a data frame on the bus where a remote request was meant.
        return unsupported("the RTR bit's position in this protocol is not known, so a remote "
                           "frame cannot be sent");
    }

    Record record;
    record.wireId = frame.isExtended
        ? ((frame.id & kExtendedIdMask) | kExtendedIdBit)
        : (((frame.id & kStandardIdMask) << kStandardIdShift) | kStandardIdBit);
    record.flags = static_cast<uint8_t>(frame.len & 0x0F);
    std::copy_n(frame.data.begin(), frame.len, record.data.begin());
    record.timestampUs = 0;
    return record;
}

// --- command builders ------------------------------------------------------

namespace
{

Frame base_frame(Code code, uint8_t tag, uint8_t field5, uint8_t reqid)
{
    Frame frame;
    frame.cmd = static_cast<uint8_t>((tag << 5) | static_cast<uint8_t>(code));
    frame.field5 = field5;
    frame.reqid = reqid;
    return frame;
}

} // namespace

Frame make_open(uint8_t reqid, uint16_t protocolVersion, uint8_t tag)
{
    // field5 is 0xFF before a handle exists -- there is nothing to echo yet.
    Frame frame = base_frame(Code::Open, tag, 0xFF, reqid);
    put_be16(frame.payload, protocolVersion);
    return frame;
}

Frame make_unlock(uint8_t reqid, uint16_t protocolVersion)
{
    Frame frame = base_frame(Code::Open, kManagementTag, 0xFF, reqid);
    put_be16(frame.payload, protocolVersion);
    return frame;
}

Frame make_version(uint8_t busHandle, uint8_t reqid, uint8_t tag)
{
    return base_frame(Code::Version, tag, busHandle, reqid);
}

Frame make_poll(uint8_t busHandle, uint8_t reqid, uint8_t tag)
{
    return base_frame(Code::Poll, tag, busHandle, reqid);
}

Frame make_filter(uint8_t busHandle, uint8_t reqid, uint32_t pattern, uint32_t mask, uint8_t index,
                  uint8_t tag)
{
    Frame frame = base_frame(Code::Filter, tag, busHandle, reqid);
    put_be32(frame.payload, pattern);
    put_be32(frame.payload, mask);
    frame.payload.push_back(index);
    // Three trailing zeroes whose meaning is not known; the captures always
    // have them and the length is fixed at twelve.
    frame.payload.push_back(0);
    frame.payload.push_back(0);
    frame.payload.push_back(0);
    return frame;
}

Frame make_accept_all_filter(uint8_t busHandle, uint8_t reqid, uint8_t index, uint8_t tag)
{
    // Mask bits are don't-care, so all ones matches everything regardless of
    // the pattern.
    return make_filter(busHandle, reqid, 0x00000000u, 0xFFFFFFFFu, index, tag);
}

Frame make_rx_subscribe(uint8_t busHandle, uint8_t reqid, uint8_t tag)
{
    Frame frame = base_frame(Code::Rx, tag, busHandle, reqid);
    frame.payload = { 0xFF, 0xFF, 0xFF, 0x01 };
    return frame;
}

Frame make_tx(uint8_t busHandle, uint8_t reqid, std::span<const Record> records, uint8_t tag)
{
    Frame frame = base_frame(Code::Tx, tag, busHandle, reqid);
    frame.data = pack_records(records);
    put_be16(frame.payload, static_cast<uint16_t>(frame.data.size()));
    put_be16(frame.payload, static_cast<uint16_t>(records.size()));
    return frame;
}

// --- stream reassembly -----------------------------------------------------

void FrameReader::push(std::span<const uint8_t> bytes)
{
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void FrameReader::compact()
{
    if (consumed_ == 0)
    {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(consumed_));
    consumed_ = 0;
}

void FrameReader::reset()
{
    buffer_.clear();
    consumed_ = 0;
}

std::optional<Frame> FrameReader::next()
{
    for (;;)
    {
        std::span<const uint8_t> view(buffer_);
        view = view.subspan(consumed_);

        // Resynchronise to a preamble. Only whole discarded bytes are counted,
        // so a partial preamble at the very end of the buffer waits for more
        // rather than being thrown away.
        size_t skip = 0;
        while (skip < view.size())
        {
            if (view[skip] == kPreamble0)
            {
                const size_t remaining = view.size() - skip;
                if (remaining < 3)
                {
                    // Could still become a preamble once more arrives.
                    break;
                }
                if (view[skip + 1] == kPreamble1 && view[skip + 2] == kPreamble2)
                {
                    break;
                }
            }
            ++skip;
        }

        if (skip != 0)
        {
            resyncBytes_ += skip;
            consumed_ += skip;
            view = view.subspan(skip);
        }

        if (view.size() < kMinFrameSize)
        {
            compact();
            return std::nullopt;
        }
        if (!has_preamble(view, 0))
        {
            // Only a partial preamble is left.
            compact();
            return std::nullopt;
        }

        const size_t covered = view[3];
        if (covered < kMinCoveredLength)
        {
            // Not a frame after all. Step over this preamble byte and hunt for
            // the next one rather than trusting a length that cannot be right.
            ++consumed_;
            ++resyncBytes_;
            continue;
        }

        const size_t headerSize = 3 + covered + 2;
        if (view.size() < headerSize)
        {
            compact();
            return std::nullopt;
        }

        // The CRC is checked here rather than left to decode_frame(), because
        // the two failures have opposite responses and decode_frame() cannot
        // tell them apart: a bad CRC means resynchronise, while a data block
        // that has not fully arrived means wait. Treating the second as the
        // first discards a frame that was about to be complete, and does it
        // most often on the busiest bus -- which is exactly when a long block
        // is most likely to straddle a USB packet.
        const uint16_t expected = crc16_ccitt(view.subspan(3, covered));
        const uint16_t actual = static_cast<uint16_t>(
            (static_cast<uint16_t>(view[3 + covered]) << 8) | view[3 + covered + 1]);
        if (expected != actual)
        {
            ++badCrcFrames_;
            ++consumed_;
            ++resyncBytes_;
            continue;
        }

        // Enough of a frame to ask how long its data block is.
        Frame header;
        header.cmd = view[4];
        header.field5 = view[5];
        header.reqid = view[6];
        header.payload.assign(view.begin() + 7, view.begin() + 3 + static_cast<ptrdiff_t>(covered));

        const size_t total = headerSize + data_block_length(header);
        if (view.size() < total)
        {
            compact();
            return std::nullopt;
        }

        auto decoded = decode_frame(view.first(total));
        if (!decoded.has_value())
        {
            // Unreachable in practice -- the preamble, length, CRC and block
            // length have all been checked above. Resynchronise rather than
            // assert, so a future change to decode_frame() cannot wedge a
            // reader in an infinite loop.
            ++badCrcFrames_;
            ++consumed_;
            ++resyncBytes_;
            continue;
        }

        consumed_ += total;
        compact();
        return std::move(*decoded);
    }
}

std::vector<uint8_t> strip_ftdi_status(std::span<const uint8_t> transfer, size_t packetSize)
{
    std::vector<uint8_t> out;
    if (packetSize <= kFtdiStatusPrefix)
    {
        return out;
    }
    out.reserve(transfer.size());

    for (size_t offset = 0; offset < transfer.size(); offset += packetSize)
    {
        const size_t chunk = std::min(packetSize, transfer.size() - offset);
        if (chunk <= kFtdiStatusPrefix)
        {
            // A packet with nothing but status in it: the device had no data.
            continue;
        }
        const auto payload = transfer.subspan(offset + kFtdiStatusPrefix, chunk - kFtdiStatusPrefix);
        out.insert(out.end(), payload.begin(), payload.end());
    }
    return out;
}

} // namespace can::motec
