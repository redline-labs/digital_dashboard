// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reader thread: owns the port, runs the handshake, delivers the data.
//
// The pipeline is bytes -> xbus::Framer -> xbus::ItemIterator -> the caller's
// handler, and it runs on one thread from open to close. That is not the shape
// bd992::StreamClient has, and the difference comes from the device rather
// than from taste: a BD992 configures itself over a SECOND socket while the
// first keeps streaming, so its configuration logic can live on the main
// thread. An MTi has one port and two mutually exclusive states. Whoever owns
// the port owns the handshake too, or the two race for the same bytes.
//
// So reconfiguration is a REQUEST rather than a call: requestReconfigure()
// raises a flag and the reader thread acts on it at the top of its loop,
// dropping to Config, re-checking, and returning to Measurement. A caller that
// needs to know it happened waits on the generation counter.
//
// The StreamFactory indirection is taken from bd992::StreamClient and earns
// its keep the same way: replay, reopen-after-unplug and a scripted peer on a
// pty all fall out of one seam. For a serial device the factory is "open
// /dev/tty... at N baud", and the reopen loop then handles USB re-enumeration
// exactly as the BD992's handles a receiver power cycle.

#ifndef MTI610_STREAM_CLIENT_H
#define MTI610_STREAM_CLIENT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mti610/byte_stream.h"
#include "mti610/device_session.h"
#include "mti610/error.h"
#include "mti610/output_config.h"
#include "xbus/framer.h"
#include "xbus/mtdata2.h"

namespace mti610
{

class StreamClient
{
  public:
    using StreamFactory = std::function<Result<std::unique_ptr<ByteStream>>()>;

    // One MTData2, with its items already walked. Called on the reader thread.
    //
    // The whole message is handed over rather than one item at a time, because
    // the items in it are one SAMPLE: the packet counter, the sample time and
    // the status word describe every measurement beside them, and a consumer
    // that received them separately would have to reassemble what the device
    // had already grouped.
    using DataHandler = std::function<void(const xbus::MessageView&)>;

    // Every byte read, before framing. For --dump-xbus.
    using ByteTap = std::function<void(std::span<const std::uint8_t>)>;

    struct Options
    {
        SessionOptions session;

        // What the device should be emitting. Compared against what it says it
        // is emitting, and written only if they differ.
        std::vector<OutputEntry> desiredOutputs;

        // Tried in order, then the last repeats. Capped rather than doubling
        // forever so a device replugged after an hour is picked up in seconds.
        std::vector<std::chrono::milliseconds> reopenBackoff {
            std::chrono::milliseconds(250), std::chrono::milliseconds(500),
            std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
            std::chrono::milliseconds(5000)
        };

        // How long one read waits. Doubles as the granularity of stop() and of
        // noticing a reconfigure request, so it is a latency budget rather
        // than a tuning knob.
        std::chrono::milliseconds readTimeout { std::chrono::milliseconds(200) };

        // Stop when the stream ends rather than reopening. What --replay wants
        // without --loop; a real port never ends cleanly.
        bool stopWhenStreamEnds { false };

        // Skip the Config handshake and just read. For replaying a capture,
        // where there is nothing to configure and GoToConfig would time out
        // three times before the first byte was delivered.
        bool readOnly { false };
    };

    struct Stats
    {
        std::uint64_t opens { 0 };
        std::uint64_t bytesRead { 0 };
        std::uint64_t dataMessages { 0 };
        std::uint64_t items { 0 };
        // Items whose identifier is not in XBUS_DATA_TABLE. Not an error -- an
        // item is self-delimiting -- but a non-zero value on a device that is
        // supposed to be a 610 means it is not one.
        std::uint64_t unknownItems { 0 };
        // Items whose payload did not parse. Distinct from unknownItems: this
        // is an identifier we claim to understand and got wrong.
        std::uint64_t malformedItems { 0 };
        // Bodies whose item walk stopped short.
        std::uint64_t truncatedBodies { 0 };
        // Times the device reset out from under us.
        std::uint64_t deviceResets { 0 };
        // Times the configuration was read, and of those, written.
        std::uint64_t configChecks { 0 };
        std::uint64_t configWrites { 0 };

        xbus::Framer::Stats framer {};

        std::string lastError;
    };

    StreamClient(StreamFactory factory, Options options, DataHandler onData);
    ~StreamClient();

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;
    StreamClient(StreamClient&&) = delete;
    StreamClient& operator=(StreamClient&&) = delete;

    void setByteTap(ByteTap tap);

    void start();
    void stop();

    bool running() const { return mRunning.load(); }

    // True once the handshake has completed and the device is in Measurement.
    bool measuring() const { return mMeasuring.load(); }

    // Ask the reader thread to drop to Config, re-check the configuration and
    // return to Measurement. Returns immediately; the work happens at the top
    // of the reader's next loop.
    void requestReconfigure();

    // Increments once per completed configuration pass. Compare before and
    // after requestReconfigure() to wait for one.
    std::uint64_t configGeneration() const { return mConfigGeneration.load(); }

    Stats stats() const;

    // What the device said about itself, once identified.
    DeviceInfo deviceInfo() const;

    // What the device reports it is actually sending.
    std::vector<OutputEntry> effectiveOutputs() const;

    // The last configuration comparison, for --check and the status message.
    std::vector<Change> lastChanges() const;

  private:
    void run();

    // One connection's lifetime: handshake, then stream until it ends.
    void serve(ByteStream& stream);

    // The Config-state pass. Returns false when the port died during it.
    bool handshake(DeviceSession& session);

    void handleMessage(const xbus::MessageView& message);

    StreamFactory mFactory;
    Options mOptions;
    DataHandler mOnData;
    ByteTap mByteTap;

    std::thread mThread;
    std::atomic<bool> mRunning { false };
    std::atomic<bool> mMeasuring { false };
    std::atomic<bool> mReconfigureWanted { false };
    std::atomic<std::uint64_t> mConfigGeneration { 0 };

    mutable std::mutex mStateMutex;
    Stats mStats;
    DeviceInfo mDeviceInfo;
    std::vector<OutputEntry> mEffectiveOutputs;
    std::vector<Change> mLastChanges;
};

} // namespace mti610

#endif // MTI610_STREAM_CLIENT_H
