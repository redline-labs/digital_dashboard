// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_socketcan/socketcan_frame.h"

#include "can/dlc.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace can::socketcan
{
namespace
{

void put_u32(std::span<uint8_t> out, size_t offset, uint32_t value)
{
    out[offset] = static_cast<uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t get_u32(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

} // namespace

Result<size_t> encode_frame(const helpers::CanFrame& frame, std::span<uint8_t> out)
{
    if (!frame.id_fits())
    {
        return invalid_argument(fmt::format("identifier 0x{:X} does not fit an {}-bit frame",
                                            frame.id, frame.isExtended ? 29 : 11));
    }
    if (frame.isError)
    {
        return invalid_argument("an error frame is something the controller reports, not "
                                "something that can be transmitted");
    }

    const size_t size = frame.isFD ? kFdFrameSize : kClassicFrameSize;
    if (out.size() < size)
    {
        return invalid_argument(fmt::format("a {} frame needs {} bytes, got {}",
                                            frame.isFD ? "CAN FD" : "classic CAN", size,
                                            out.size()));
    }

    const uint8_t maxLength = frame.isFD ? 64 : 8;
    if (frame.len > maxLength)
    {
        return invalid_argument(fmt::format("a {} frame carries at most {} bytes, not {}",
                                            frame.isFD ? "CAN FD" : "classic CAN", maxLength,
                                            frame.len));
    }
    if (frame.isFD)
    {
        if (frame.isRTR)
        {
            return invalid_argument("CAN FD has no remote transmission request");
        }
        if (!is_valid_can_length(frame.len, true))
        {
            return invalid_argument(fmt::format(
                "{} bytes is not a length CAN FD can express; the next size up is {}", frame.len,
                round_up_can_length(frame.len, true)));
        }
    }

    std::fill(out.begin(), out.begin() + static_cast<ptrdiff_t>(size), uint8_t { 0 });

    uint32_t id = frame.id & (frame.isExtended ? kEffMask : kSffMask);
    if (frame.isExtended)
    {
        id |= kEffFlag;
    }
    if (frame.isRTR)
    {
        id |= kRtrFlag;
    }
    put_u32(out, 0, id);

    out[4] = frame.len;

    if (frame.isFD)
    {
        // canfd_frame's byte 5 is `flags`; the classic frame's is padding the
        // kernel ignores.
        uint8_t flags = 0;
        if (frame.isBRS) flags |= kFdFlagBrs;
        if (frame.isESI) flags |= kFdFlagEsi;
        out[5] = flags;
    }

    // A remote frame has a length but no payload.
    const size_t payloadOffset = 8;
    const size_t copy = frame.isRTR ? 0 : frame.len;
    for (size_t i = 0; i < copy; ++i)
    {
        out[payloadOffset + i] = frame.data[i];
    }

    return size;
}

Result<helpers::CanFrame> decode_frame(std::span<const uint8_t> bytes)
{
    if (bytes.size() != kClassicFrameSize && bytes.size() != kFdFrameSize)
    {
        return protocol_error(fmt::format(
            "a CAN_RAW read returned {} bytes; a classic frame is {} and an FD frame is {}",
            bytes.size(), kClassicFrameSize, kFdFrameSize));
    }

    const bool fd = bytes.size() == kFdFrameSize;
    const uint32_t raw = get_u32(bytes, 0);

    helpers::CanFrame frame {};
    frame.isExtended = (raw & kEffFlag) != 0;
    frame.isRTR = (raw & kRtrFlag) != 0;
    frame.isError = (raw & kErrFlag) != 0;
    frame.isFD = fd;
    // The mask has to follow the EFF flag. An error frame's "identifier" is a
    // bitmap of what went wrong rather than an address, and the kernel puts it
    // in the low 29 bits, so it is masked the same way.
    frame.id = raw & (frame.isExtended || frame.isError ? kEffMask : kSffMask);

    const uint8_t maxLength = fd ? 64 : 8;
    frame.len = std::min<uint8_t>(bytes[4], maxLength);

    if (fd)
    {
        frame.isBRS = (bytes[5] & kFdFlagBrs) != 0;
        frame.isESI = (bytes[5] & kFdFlagEsi) != 0;
    }

    const size_t payloadOffset = 8;
    const size_t available = bytes.size() - payloadOffset;
    const size_t copy = frame.isRTR ? 0 : std::min<size_t>(frame.len, available);
    for (size_t i = 0; i < copy; ++i)
    {
        frame.data[i] = bytes[payloadOffset + i];
    }

    return frame;
}

} // namespace can::socketcan
