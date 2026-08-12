// SPDX-License-Identifier: GPL-3.0-or-later

#include "gsof/framer.h"

#include <algorithm>

namespace gsof
{

Framer::Framer(std::size_t maxBuffer) :
    mMaxBuffer(std::max<std::size_t>(maxBuffer, trimcomm::kMaxPacketSize))
{
    mBuffer.reserve(trimcomm::kMaxPacketSize * 2);
}

void Framer::push(std::span<const std::uint8_t> bytes)
{
    // Compact first. Doing it here rather than at the end of next() is what
    // keeps a returned PacketView valid until the caller comes back for
    // another one.
    if (mConsumed != 0)
    {
        mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(mConsumed));
        mConsumed = 0;
    }

    mBuffer.insert(mBuffer.end(), bytes.begin(), bytes.end());

    if (mBuffer.size() > mMaxBuffer)
    {
        // Nothing in here is a packet: a packet is at most 261 bytes and we
        // scan from the front, so reaching the cap means we never found a
        // plausible STX in far more data than one could span. Keeping the tail
        // would only delay the same conclusion.
        mStats.droppedBytes += mBuffer.size();
        ++mStats.overflows;
        mBuffer.clear();
    }
}

void Framer::resync()
{
    ++mStats.resyncs;

    // Skip the byte we already know cannot start a packet, then look for the
    // next candidate. Anything before it is not recoverable.
    std::size_t search = mConsumed + 1;
    while (search < mBuffer.size() && mBuffer[search] != trimcomm::kStx)
    {
        ++search;
    }

    mStats.droppedBytes += search - mConsumed;
    mConsumed = search;
}

std::optional<trimcomm::PacketView> Framer::next()
{
    while (mConsumed < mBuffer.size())
    {
        const std::span<const std::uint8_t> window(mBuffer.data() + mConsumed, mBuffer.size() - mConsumed);

        const Result<trimcomm::PacketView> packet = trimcomm::parse_packet(window);

        if (packet.has_value())
        {
            mConsumed += trimcomm::packet_size(static_cast<std::uint8_t>(packet->data.size()));
            ++mStats.packets;
            return *packet;
        }

        switch (packet.error().kind)
        {
            case ErrorKind::Truncated:
                // Routine: the rest of this packet has not arrived. Note that
                // this is only reached once the front byte IS an STX, so we
                // are waiting on a plausible packet rather than on garbage.
                return std::nullopt;

            case ErrorKind::BadChecksum:
                ++mStats.checksumErrors;
                break;

            case ErrorKind::BadFraming:
                // Either the front byte is not STX (we are mid-hunt) or the
                // ETX is not where the length claimed. Only the second is a
                // framing *error* worth counting; the first is the hunt doing
                // its job, and counting it would report one error per garbage
                // byte.
                if (mBuffer[mConsumed] == trimcomm::kStx)
                {
                    ++mStats.framingErrors;
                }
                break;

            case ErrorKind::LengthMismatch:
            case ErrorKind::UnknownRecord:
            case ErrorKind::PageOutOfOrder:
            case ErrorKind::TooLong:
                // parse_packet cannot produce these. Listed so that adding an
                // ErrorKind fails to compile here rather than silently falling
                // into whichever branch happens to be last.
                break;
        }

        resync();
    }

    return std::nullopt;
}

void Framer::reset()
{
    mBuffer.clear();
    mConsumed = 0;
}

} // namespace gsof
