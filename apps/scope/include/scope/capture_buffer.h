#ifndef SCOPE_CAPTURE_BUFFER_H_
#define SCOPE_CAPTURE_BUFFER_H_

#include "bag/queue.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace scope
{

// Everything on the bus, held in memory, bounded, evicting oldest.
//
// WHY BOUNDED, AND BY TWO THINGS. With CarPlay streaming, the bus runs about
// 1.5 GB/hour -- H.264 access units plus raw PCM -- so an unbounded capture is
// OOM-killed within minutes, taking the capture with it. Telemetry alone is
// around 11 MB/hour, where a byte cap would never bite and half an hour is the
// limit that matters. Neither bound alone is right for both, so both apply and
// whichever binds first wins.
//
// EVICTION IS COUNTED, not silent. A capture quietly dropping its head is the
// same class of lie as a recorder dropping samples: a trace that starts partway
// through reads as a publisher that had not started yet. `bag record` already
// treats it that way -- the count lands in metadata.yaml and is warned about --
// and the transport bar reports evicted() beside the retained span for the same
// reason.
//
// Messages are `bag::QueuedMessage` because that type already owns its payload,
// which is not an optimisation but a requirement: the views a zenoh callback is
// handed are valid only for the duration of the call.
class CaptureBuffer
{
  public:
    // `max_bytes` counts payload bytes plus the per-message string fields, not
    // allocator overhead -- close enough to keep a process inside a memory
    // budget, and cheap enough to compute on the producer thread.
    CaptureBuffer(std::size_t max_bytes, double max_seconds);

    // Producer: a zenoh RX thread. Never blocks for longer than the mutex, and
    // never allocates beyond the move.
    void push(bag::QueuedMessage message);

    // Change the bounds in place, evicting immediately if the new ones are
    // tighter.
    //
    // In place rather than by rebuilding the recorder, because the bounds
    // arrive with the workspace -- after capture has already started -- and
    // rebuilding would throw away everything captured before the file was
    // opened. Loosening them does not bring back what was already evicted;
    // nothing can.
    void setBounds(std::size_t max_bytes, double max_seconds);

    // Everything currently retained, oldest first, visited under the lock.
    //
    // The callback runs WHILE THE LOCK IS HELD, so it must not push, must not
    // block and must not call back into this object. That is the trade for
    // handing out references instead of copying a gigabyte of payloads: the
    // provider that reads this decodes one signal per pass and copies only the
    // samples it produces.
    //
    // The lock is RELEASED AND RETAKEN periodically inside the walk, so a full
    // decode pass no longer starves the RX thread's push() or the transport
    // bar's stat reads for its whole duration. Consequence: a message pushed or
    // evicted mid-pass may or may not be visited -- the same weak snapshot a
    // bag growing under a reader gives. References handed to the callback are
    // still valid only for that call.
    // The visitor returns true to continue, false to stop the walk early.
    void forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                 const std::function<bool(const bag::QueuedMessage&)>& visit) const;

    // Every transport-bar number in ONE lock acquisition. The bar reads four of
    // these per render tick; taking the mutex once instead of four times is
    // less contention against the RX thread for the same answer.
    struct Stats
    {
        std::size_t messages = 0;
        std::size_t bytes = 0;
        std::uint64_t evicted = 0;
        std::uint64_t revision = 0;
        double retained_span_seconds = 0.0;
    };
    Stats stats() const;

    // [first, last] log_time of what is retained, in nanoseconds since the UNIX
    // epoch. {0, 0} when empty.
    std::pair<std::uint64_t, std::uint64_t> spanNanos() const;

    // How many retained messages fall in each of `buckets` equal slices of
    // [t0_ns, t1_ns]. `out` is resized and fully overwritten, so a caller can
    // keep one vector across frames.
    //
    // NOT PER FRAME. It is O(retained) under the same mutex the zenoh RX thread
    // needs to push -- half an hour of a busy bus is millions of entries, and
    // holding that lock at the render rate would stall the producer rather than
    // just cost the consumer. The overview strip recomputes on a throttle and
    // draws the last answer in between.
    //
    // Bucket i covers [t0 + i*dt, t0 + (i+1)*dt), with the last closed at the
    // top -- the same convention decimateMinMax uses, so the strip and the plot
    // never disagree about which side of a boundary a message fell on.
    void density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                 std::vector<std::uint32_t>& out) const;

    // Bumped on every push and every eviction, so a reader can tell whether the
    // window it is showing still describes what is here.
    std::uint64_t revision() const;

    std::size_t size() const;
    std::size_t bytes() const;

    // How much was thrown away to stay inside the bounds. Monotonic.
    std::uint64_t evicted() const;
    std::uint64_t evictedBytes() const;

    // Seconds between the oldest and newest retained message. Reported next to
    // evicted() in the transport bar: together they say exactly how much of the
    // session is still reviewable.
    double retainedSpanSeconds() const;

    void clear();

  private:
    // Payload plus the owned strings. Allocator overhead is deliberately not
    // counted -- it would make the bound depend on the allocator rather than on
    // the data, and the point of the cap is to stay inside a memory budget, not
    // to be exact.
    static std::size_t sizeOf(const bag::QueuedMessage& message);

    // Called with the lock held.
    void evictLocked();

    mutable std::mutex mutex_;
    std::deque<bag::QueuedMessage> messages_;

    std::size_t max_bytes_;
    double max_seconds_;

    std::size_t bytes_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t evicted_ = 0;
    std::uint64_t evicted_bytes_ = 0;
};

}  // namespace scope

#endif  // SCOPE_CAPTURE_BUFFER_H_
