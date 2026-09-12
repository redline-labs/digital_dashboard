// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns a byte stream into whole, checksum-verified XBus messages.
//
// This is the one piece of the protocol library that is not constexpr, because
// it owns a buffer across calls: a serial port hands you arbitrary fragments,
// and at 115200 baud a 60-byte MTData2 routinely spans several reads.
// Validation is still parse_message(), so the rules live in one place.
//
// RESYNCHRONISATION IS THE POINT OF THIS CLASS, and XBus makes it sharper than
// most. There is no escaping and no trailer: 0xFA is an ordinary byte inside a
// payload, and an accelerometer reading passes through 0xFA several times a
// second. So "scan forward to the next 0xFA" is not a way of finding a message
// boundary -- only the length and the checksum together decide that, and the
// only safe recovery from a failed candidate is to drop exactly one byte and
// try again. Scanning to the next preamble instead would skip real messages
// whose header happens to sit behind a payload byte of 0xFA.
//
// Resyncs are counted rather than logged, because the interesting question is
// never "did one happen" -- it is "how many per hour", which is a number the
// node publishes in its status message.

#ifndef XBUS_FRAMER_H
#define XBUS_FRAMER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xbus/message.h"

namespace xbus
{

class Framer
{
  public:
    struct Stats
    {
        // Messages handed out of next().
        std::uint64_t messages { 0 };
        // Messages whose framing was right but whose checksum was not. A
        // non-zero value here with a healthy message count is a noisy link or
        // a baud-rate mismatch; a non-zero value with a zero message count is
        // usually the wrong baud rate outright.
        std::uint64_t checksumErrors { 0 };
        // Candidates rejected for a length this protocol cannot carry, or for
        // an extended message id (see parse_message).
        std::uint64_t unsupported { 0 };
        // How many times the scanner had to drop a byte and retry.
        std::uint64_t resyncs { 0 };
        // Bytes thrown away while hunting. `resyncs` counts events, this
        // counts cost -- one resync that discarded 40 kB is a different
        // problem from 400 that discarded one byte each.
        std::uint64_t droppedBytes { 0 };
        // Times the buffer hit its cap and was cleared. Should stay zero; a
        // non-zero value means the far end is not speaking XBus at all.
        std::uint64_t overflows { 0 };
    };

    // Far above the largest message the protocol allows, so it is only reached
    // by a stream that is not XBus, or by a declared length that will never be
    // satisfied. Big enough that a burst never trips it, small enough that a
    // garbage producer cannot exhaust memory.
    static constexpr std::size_t kDefaultMaxBuffer = 64 * 1024;

    explicit Framer(std::size_t maxBuffer = kDefaultMaxBuffer);

    // Append received bytes. Invalidates any MessageView previously returned.
    void push(std::span<const std::uint8_t> bytes);

    // The next complete message, or nullopt when more bytes are needed.
    //
    // The returned view points into this object's buffer and stays valid until
    // the next push() or reset() -- next() only advances a read cursor, it
    // never moves the bytes. That is deliberate, and it is why compaction
    // happens at the top of push() rather than at the end of next(): a caller
    // may drain a whole read into a container of views before touching any of
    // them.
    //
    //     framer.push(justRead);
    //     while (auto message = framer.next()) { handle(*message); }
    std::optional<MessageView> next();

    // Discard everything buffered. Called when the port is reopened: bytes
    // from before a disconnect cannot be part of a message that arrives after
    // it.
    void reset();

    const Stats& stats() const { return mStats; }

    // Bytes currently held, for tests and for the status message.
    std::size_t buffered() const { return mBuffer.size() - mConsumed; }

  private:
    // Drop exactly one byte. See the note on resynchronisation above for why
    // this does not scan ahead to the next preamble.
    void resync();

    std::vector<std::uint8_t> mBuffer;
    // Bytes at the front of mBuffer already handed out. Compaction is deferred
    // so that the view returned by next() stays valid until the following call.
    std::size_t mConsumed { 0 };
    std::size_t mMaxBuffer;
    Stats mStats {};
};

} // namespace xbus

#endif // XBUS_FRAMER_H
