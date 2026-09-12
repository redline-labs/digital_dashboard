// SPDX-License-Identifier: GPL-3.0-or-later

#include "xbus/framer.h"

#include <algorithm>

namespace xbus
{

Framer::Framer(std::size_t maxBuffer) :
    mMaxBuffer(std::max<std::size_t>(maxBuffer, kMaxMessageSize))
{
    // Two standard-length messages, which is what a 115200-baud read actually
    // delivers. An extended-length MTData2 grows the vector once and then it
    // stays grown.
    mBuffer.reserve((kMaxStandardDataSize + kHeaderSize + kChecksumSize) * 2);
}

void Framer::push(std::span<const std::uint8_t> bytes)
{
    // Compact first. Doing it here rather than at the end of next() is what
    // keeps a returned MessageView valid until the caller comes back for
    // another one.
    if (mConsumed != 0)
    {
        mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(mConsumed));
        mConsumed = 0;
    }

    mBuffer.insert(mBuffer.end(), bytes.begin(), bytes.end());

    if (mBuffer.size() > mMaxBuffer)
    {
        // We scan from the front, so reaching the cap means no candidate in
        // far more data than one message can span ever passed its checksum.
        // Keeping the tail would only delay the same conclusion.
        mStats.droppedBytes += mBuffer.size();
        ++mStats.overflows;
        mBuffer.clear();
        mConsumed = 0;
    }
}

void Framer::resync()
{
    ++mStats.resyncs;
    ++mStats.droppedBytes;
    ++mConsumed;
}

std::optional<MessageView> Framer::next()
{
    while (mConsumed < mBuffer.size())
    {
        const std::span<const std::uint8_t> window(mBuffer.data() + mConsumed,
                                                   mBuffer.size() - mConsumed);

        const Result<MessageView> message = parse_message(window);

        if (message.has_value())
        {
            mConsumed += message_size(message->data.size());
            ++mStats.messages;
            return *message;
        }

        switch (message.error().kind)
        {
            case ErrorKind::Truncated:
                // Routine: the rest of this message has not arrived. Only
                // reached once the front byte IS a preamble and the declared
                // length is plausible, so we are waiting on a candidate rather
                // than on garbage.
                return std::nullopt;

            case ErrorKind::BadChecksum:
                ++mStats.checksumErrors;
                break;

            case ErrorKind::BadFraming:
                // The front byte is not a preamble. This is the hunt doing its
                // job, not an error -- counting it would report one error per
                // garbage byte.
                break;

            case ErrorKind::TooLong:
            case ErrorKind::UnsupportedFormat:
                ++mStats.unsupported;
                break;

            case ErrorKind::LengthMismatch:
            case ErrorKind::UnknownDataId:
            case ErrorKind::DeviceReportedError:
                // parse_message cannot produce these; they belong to the
                // MTData2 layer above. Listed so that adding an ErrorKind
                // fails to compile here rather than silently falling into
                // whichever branch happens to be last.
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
    // Stats deliberately survive: they describe the link, not the connection,
    // and a node that zeroed them on every reopen would report a clean link
    // precisely when the link is at its worst.
}

} // namespace xbus
