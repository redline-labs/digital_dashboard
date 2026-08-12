// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GSOF stream: one thread reading bytes and turning them into records.
//
// The pipeline is bytes -> Framer -> PageAssembler -> RecordIterator, and it
// is the same pipeline whether the bytes came from a socket or from a captured
// file, because it is written against ByteStream. That is what makes
// `bd992 --replay` a real test of the decode path rather than a demo.
//
// RECONNECTION IS THIS CLASS'S JOB, and it is not optional. A GNSS receiver on
// a vehicle loses power with the ignition, and a node that needed restarting
// every time would be useless. The reader thread owns the whole cycle: connect,
// read until the link fails, back off, connect again. Nothing above it sees a
// socket, so nothing above it has to know a reconnection happened.
//
// Both the framer and the assembler are RESET on reconnect. Half a packet from
// before a drop cannot be part of a packet that arrives after it, and a
// transmission missing its final pages must not be completed by pages from a
// different session -- that would splice two unrelated payloads into one that
// still parses.

#ifndef BD992_STREAM_CLIENT_H
#define BD992_STREAM_CLIENT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "bd992/byte_stream.h"
#include "bd992/error.h"
#include "gsof/framer.h"
#include "gsof/record_iterator.h"
#include "gsof/transport.h"

namespace bd992
{

class StreamClient
{
  public:
    // How to obtain a stream. Called on the reader thread, once per connection
    // attempt. Taking a factory rather than a host and port is what lets the
    // tests and `--replay` substitute a different source without this class
    // knowing anything about sockets.
    using StreamFactory = std::function<Result<std::unique_ptr<ByteStream>>()>;

    // Called on the reader thread for each record in a completed
    // transmission, known or not. Must not block: the receiver keeps sending
    // while this runs.
    using RecordHandler = std::function<void(const gsof::RawRecord&)>;

    // Called after the last record of a transmission. The node uses it to
    // stamp one arrival time across records that were sent together.
    using TransmissionHandler = std::function<void()>;

    // Every byte received, before framing. For --dump-gsof.
    using ByteTap = std::function<void(std::span<const std::uint8_t>)>;

    struct Options
    {
        // Tried in order, then the last one repeats. Capped rather than
        // unbounded so a receiver that comes back after an hour is picked up
        // within seconds rather than after another hour of doubling.
        std::vector<std::chrono::milliseconds> reconnectBackoff {
            std::chrono::milliseconds(250),
            std::chrono::milliseconds(500),
            std::chrono::milliseconds(1000),
            std::chrono::milliseconds(2000),
            std::chrono::milliseconds(5000),
        };

        // How long a read waits before returning empty-handed. Also the
        // granularity at which stop() is noticed, which is why it is short.
        std::chrono::milliseconds readTimeout { 200 };

        // Stop after the stream ends rather than reconnecting. What --replay
        // over a file without --loop wants.
        bool stopWhenStreamEnds { false };
    };

    struct Stats
    {
        bool connected { false };
        std::uint64_t connects { 0 };
        std::uint64_t connectFailures { 0 };
        std::uint64_t disconnects { 0 };
        std::uint64_t bytesRead { 0 };

        // Transmissions completed, and the records pulled out of them.
        std::uint64_t transmissions { 0 };
        std::uint64_t records { 0 };
        // Records whose type is not in GSOF_RECORD_TABLE. Counted rather than
        // logged per occurrence: at 10 Hz an unmodelled record would otherwise
        // fill a log with the same line.
        std::uint64_t unknownRecords { 0 };
        // Records whose type IS known but whose body did not decode. This one
        // is a bug or a firmware change, not a configuration choice.
        std::uint64_t malformedRecords { 0 };

        gsof::Framer::Stats framer {};
        gsof::PageAssembler::Stats assembler {};

        // Empty unless the last connection attempt failed.
        std::string lastError;
    };

    StreamClient(StreamFactory factory, Options options, RecordHandler onRecord);
    ~StreamClient();

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // Optional extras. Set before start().
    void setTransmissionHandler(TransmissionHandler handler);
    void setByteTap(ByteTap tap);

    void start();
    void stop();

    // True once start() has been called and the reader thread has not exited.
    // A stopWhenStreamEnds client goes false on its own when the file ends.
    bool isRunning() const { return mRunning.load(); }

    Stats stats() const;

  private:
    void run();
    // Feed one read's worth of bytes through the pipeline. Separate from run()
    // so the byte path can be exercised without a thread.
    void consume(std::span<const std::uint8_t> bytes);
    void deliverTransmission(std::span<const std::uint8_t> payload);

    StreamFactory mFactory;
    Options mOptions;
    RecordHandler mOnRecord;
    TransmissionHandler mOnTransmission;
    ByteTap mByteTap;

    gsof::Framer mFramer;
    gsof::PageAssembler mAssembler;

    std::thread mThread;
    std::atomic<bool> mRunning { false };
    std::atomic<bool> mStopping { false };

    // Guards mStats only. The reader thread writes it; stats() reads it from
    // whichever thread publishes the status message.
    mutable std::mutex mStatsMutex;
    Stats mStats;
};

} // namespace bd992

#endif // BD992_STREAM_CLIENT_H
