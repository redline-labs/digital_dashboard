// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording one bridged channel to a PCAN .trc file.
//
// Why this is here rather than behind a `trc:` channel in the CAN library: a
// can::Channel::send() only ever sees the frames *this process transmits*. A
// trace worth keeping has both directions of the bus in it, and the only place
// both exist is the bridge, between the pump thread that receives and the zenoh
// callback that sends. So recording is a tap on a channel, not a backend.
//
// Two properties this has to hold, and both are about not disturbing what it is
// recording:
//
//   * Neither caller may touch the file. record_rx() runs on the pump thread,
//     where a blocking write means CAN frames pile up in the driver and get
//     dropped; record_tx() runs on a zenoh callback thread, which this codebase
//     requires never to block. Both do arithmetic and a push, and a dedicated
//     thread does the I/O.
//
//   * A full queue drops the newest frame and counts it. A trace with a hole
//     you can see beats one that quietly reordered or blocked, because the
//     whole reason to keep a trace is to trust its timing.
//
// The time base is the subtle part. Frame timestamps in this tree do not share
// an epoch -- SocketCAN gives the kernel's wall clock, a PCAN adapter gives its
// own uptime, a virtual bus gives nothing at all -- so an absolute stamp cannot
// be used directly. Differences can: whatever the origin, the gap between two
// frames from one adapter is real. So the recorder pins its own origin at the
// first hardware-stamped frame and measures every later one from there, falling
// back to arrival time for transmitted frames and for channels that supply no
// stamps. See offset_for() in the implementation.
#ifndef CAN_BRIDGE_TRC_RECORDER_H
#define CAN_BRIDGE_TRC_RECORDER_H

#include "can_trc/trc.h"

#include "helpers/can_frame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace can_bridge
{

class TrcRecorder
{
public:
    // `bus` is what goes in the trace's Bus column, so several channels
    // recorded from one bridge stay distinguishable. `busInfo` names the
    // channel in the file's header table.
    static can::Result<std::unique_ptr<TrcRecorder>> create(const std::string& path, uint8_t bus,
                                                            const can::trc::BusInfo& busInfo);

    ~TrcRecorder();

    TrcRecorder(const TrcRecorder&) = delete;
    TrcRecorder& operator=(const TrcRecorder&) = delete;

    // Called from the pump thread. Never blocks on the file.
    void record_rx(const helpers::CanFrame& frame);
    // Called from a zenoh subscriber callback. Never blocks on the file.
    void record_tx(const helpers::CanFrame& frame);

    uint64_t recorded() const { return recorded_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    const std::string& path() const { return path_; }

private:
    TrcRecorder() = default;

    void enqueue(const helpers::CanFrame& frame, bool isTx);
    // Where on the trace's timeline this frame goes. Called on the producing
    // thread, deliberately: computing it on the writer thread would fold this
    // queue's own latency into the timing the trace is supposed to preserve.
    // Caller holds mutex_.
    uint64_t offset_for(const helpers::CanFrame& frame, bool isTx);
    void run();

    std::unique_ptr<can::trc::Writer> writer_;
    std::string path_;
    uint8_t bus_ { 1 };

    // A busy 500 kbit/s bus is about 4000 frames a second, so this holds a few
    // seconds of one against a stalled disk.
    static constexpr size_t kQueueDepth = 16384;

    std::mutex mutex_;
    std::condition_variable wakeup_;
    std::deque<can::trc::Record> queue_;
    bool stopping_ { false };

    std::thread writerThread_;

    std::atomic<uint64_t> recorded_ { 0 };
    std::atomic<uint64_t> dropped_ { 0 };

    // Timeline state, guarded by mutex_ along with the queue itself: a record's
    // offset and its position in the queue have to be decided together or two
    // producers can swap places between the two and write a trace whose offsets
    // run backwards.
    std::chrono::steady_clock::time_point startSteady_;
    uint64_t number_ { 0 };
    // The first frame that arrived with a hardware timestamp, and where on this
    // trace's timeline it landed. Everything stamped afterwards is measured
    // from that pair.
    bool haveHardwareOrigin_ { false };
    uint64_t hardwareOriginUs_ { 0 };
    uint64_t hardwareOriginOffsetUs_ { 0 };
};

} // namespace can_bridge

#endif // CAN_BRIDGE_TRC_RECORDER_H
