// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns a byte stream into whole, checksum-verified DCOL packets.
//
// This is the one piece that is not constexpr, because it owns a buffer across
// calls: TCP hands you arbitrary fragments, and a packet routinely spans two
// reads. The validation it performs is still trimcomm::parse_packet, so the
// rules live in exactly one place.
//
// THE POINT OF THIS CLASS IS RESYNCHRONISATION, and it is worth saying why.
// The reference ROS driver's equivalent has none: on a checksum mismatch or a
// missing ETX it leaves its buffer untouched and re-examines the same bytes on
// the next read, so a single corrupted byte wedges the stream permanently. The
// symptom is a GNSS feed that simply stops, with no error and no recovery short
// of restarting the process. Here, any failure drops one byte and rescans for
// the next STX, which costs at most one packet and always recovers.
//
// Resyncs are counted rather than logged, because the interesting question is
// never "did one happen" -- it is "how many per hour", which is a number the
// node publishes in its status message.

#ifndef GSOF_FRAMER_H
#define GSOF_FRAMER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gsof/trimcomm.h"

namespace gsof
{

class Framer
{
  public:
    struct Stats
    {
        // Packets handed out of next().
        std::uint64_t packets { 0 };
        // Packets whose framing was right but whose checksum was not. A
        // non-zero value here with a healthy packet count is a noisy link; a
        // non-zero value with a zero packet count is usually the wrong port.
        std::uint64_t checksumErrors { 0 };
        // Packets whose ETX was not where the length byte said.
        std::uint64_t framingErrors { 0 };
        // How many times the scanner had to hunt for a new STX.
        std::uint64_t resyncs { 0 };
        // Bytes thrown away while hunting. resyncs counts events, this counts
        // cost -- one resync that discarded 40 kB is a different problem from
        // 400 that discarded one byte each.
        std::uint64_t droppedBytes { 0 };
        // Times the buffer hit its cap and was cleared. Should stay zero; a
        // non-zero value means the far end is sending something that is not
        // DCOL at all.
        std::uint64_t overflows { 0 };
    };

    // The default cap is far above the 261-byte maximum packet, so it is only
    // reached by a stream that is not DCOL. Big enough that a burst never
    // trips it, small enough that a garbage producer cannot exhaust memory.
    static constexpr std::size_t kDefaultMaxBuffer = 64 * 1024;

    explicit Framer(std::size_t maxBuffer = kDefaultMaxBuffer);

    // Append received bytes. Invalidates any PacketView previously returned.
    void push(std::span<const std::uint8_t> bytes);

    // The next complete packet, or nullopt when more bytes are needed.
    //
    // The returned view points into this object's buffer and stays valid until
    // the next push() or reset() -- next() only advances a read cursor, it
    // never moves the bytes. That is deliberate, and it is why compaction
    // happens at the top of push() rather than at the end of next(): it means
    // a caller may drain a whole read into a container of views before
    // touching any of them.
    //
    //     framer.push(justRead);
    //     while (auto packet = framer.next()) { handle(*packet); }
    std::optional<trimcomm::PacketView> next();

    // Discard everything buffered. Called on reconnect: bytes from before a
    // dropped connection cannot be part of a packet that arrives after it.
    void reset();

    const Stats& stats() const { return mStats; }

    // Bytes currently held, for tests and for the status message.
    std::size_t buffered() const { return mBuffer.size() - mConsumed; }

  private:
    // Drop bytes up to and including the front one, then jump to the next STX
    // if there is one. Counts the event and the cost.
    void resync();

    std::vector<std::uint8_t> mBuffer;
    // Bytes at the front of mBuffer already handed out. Compaction is deferred
    // so that the view returned by next() stays valid until the following call.
    std::size_t mConsumed { 0 };
    std::size_t mMaxBuffer;
    Stats mStats {};
};

} // namespace gsof

#endif // GSOF_FRAMER_H
