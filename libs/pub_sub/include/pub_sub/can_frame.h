#ifndef PUB_SUB_CAN_FRAME_H_
#define PUB_SUB_CAN_FRAME_H_

#include "can_frame.capnp.h"
#include "helpers/can_frame.h"

#include <kj/common.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pub_sub
{

// A frame off the bus topic, as the in-process struct every CAN consumer uses.
//
// `len` comes back as the number of bytes actually copied: the smallest of the
// declared length, the list the publisher supplied, and the 64 bytes a frame can
// hold. A publisher that claimed more than it sent must not hand a decoder
// uninitialised bytes, and a frame shorter than the message it claims to be has
// to reach the decoder short so that it is rejected rather than read as padding.
// Eight nodes used to spell this copy out by hand, each with its own casts.
inline helpers::CanFrame fromCapnp(::CanFrame::Reader message)
{
    helpers::CanFrame frame;
    frame.id = message.getId();
    frame.isExtended = message.getExtended();
    frame.isRTR = message.getRtr();
    frame.isFD = message.getFd();
    frame.isBRS = message.getBrs();
    frame.isESI = message.getEsi();
    frame.isError = message.getError();
    frame.timestampUs = message.getTimestampUs();

    const std::size_t limit = std::min<std::size_t>(frame.data.size(), message.getLen());
    std::size_t copied = 0u;
    for (const std::uint8_t byte : message.getData())
    {
        if (copied == limit)
        {
            break;
        }
        frame.data[copied] = byte;
        ++copied;
    }
    // copied <= 64, so it fits the field.
    frame.len = static_cast<std::uint8_t>(copied);
    return frame;
}

// The in-process struct onto a bus-topic builder. Sets every field the schema
// shares with helpers::CanFrame, and never `channel`, which only the publisher
// knows. The list is written from data_span(), so a `len` past the buffer
// cannot read beyond it.
inline void toCapnp(const helpers::CanFrame& frame, ::CanFrame::Builder message)
{
    const auto payload = frame.data_span();
    message.setId(frame.id);
    // data_span() is at most 64 bytes, so it fits the field.
    message.setLen(static_cast<std::uint8_t>(payload.size()));
    message.setExtended(frame.isExtended);
    message.setRtr(frame.isRTR);
    message.setFd(frame.isFD);
    message.setBrs(frame.isBRS);
    message.setEsi(frame.isESI);
    message.setError(frame.isError);
    message.setTimestampUs(frame.timestampUs);
    message.setData(kj::arrayPtr(payload.data(), payload.size()));
}

} // namespace pub_sub

#endif // PUB_SUB_CAN_FRAME_H_
