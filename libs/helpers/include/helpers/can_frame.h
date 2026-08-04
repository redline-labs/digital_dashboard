// SPDX-License-Identifier: GPL-3.0-or-later
//
// One CAN frame, classic or FD, as it moves through this repository.
//
// The flags below are not decoration. Without `isExtended` an 11-bit 0x123 and
// a 29-bit 0x123 are indistinguishable, and they are different messages on the
// same bus -- a receiver that conflates them will happily decode one as the
// other. Without `isError` a bus-off report from the controller looks like an
// ordinary message from a device that does not exist. These matter as soon as
// frames come from real hardware rather than from a log replay, which is why
// they were not needed until now.
#ifndef HELPERS_CAN_FRAME_H
#define HELPERS_CAN_FRAME_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace helpers
{

struct CanFrame
{
    // 11-bit when isExtended is false, 29-bit when it is true.
    uint32_t id;
    // Payload length in bytes: 0..8 for classic, and one of
    // 0..8/12/16/20/24/32/48/64 for FD. This is a real length, not a DLC code.
    uint8_t len;
    bool isExtended;
    bool isFD;

    // Remote transmission request: a request for someone else to send this
    // identifier. Carries no payload, and does not exist in CAN FD.
    bool isRTR;
    // FD bit-rate switch: the data phase ran at the faster rate.
    bool isBRS;
    // FD error state indicator: the transmitter was error-passive.
    bool isESI;
    // Not a message at all -- the controller reporting a bus condition. `id`
    // and `data` carry backend-specific detail, so treat this as a signal to
    // look at the channel's statistics rather than something to decode.
    bool isError;

    // When the frame was seen, in microseconds, from whatever clock the
    // backend has. Hardware timestamps come from the adapter and are far more
    // useful than an arrival time taken after USB and scheduling latency; zero
    // means the backend did not supply one.
    uint64_t timestampUs;

    std::array<uint8_t, 64> data;

    CanFrame()
        : id { 0u }
        , len { 0u }
        , isExtended { false }
        , isFD { false }
        , isRTR { false }
        , isBRS { false }
        , isESI { false }
        , isError { false }
        , timestampUs { 0u }
        , data { { 0 } }
    {
    }

    std::span<const uint8_t> data_span() const
    {
        const size_t span_size = std::min(static_cast<size_t>(len), data.size());
        return std::span<const uint8_t>(data.begin(), span_size);
    }

    std::span<uint8_t> data_span()
    {
        const size_t span_size = std::min(static_cast<size_t>(len), data.size());
        return std::span<uint8_t>(data.begin(), span_size);
    }

    // The largest identifier this frame's format allows.
    uint32_t id_mask() const { return isExtended ? 0x1FFFFFFFu : 0x7FFu; }

    bool id_fits() const { return id <= id_mask(); }
};

} // namespace helpers

#endif // HELPERS_CAN_FRAME_H
