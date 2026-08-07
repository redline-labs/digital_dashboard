#ifndef SCOPE_RAW_BUFFER_H_
#define SCOPE_RAW_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace scope
{

// One message's bytes, on the source's clock. The raw analogue of Sample.
//
// OWNED, not viewed. The payload a zenoh callback is handed is valid only for
// the duration of the call and a bag reader's only until the next message
// replaces its chunk, so the copy is not an inefficiency to remove later -- it
// is the reason this type exists.
struct RawMessage
{
    double t = 0.0;

    std::vector<std::uint8_t> payload;

    // Whatever the consumer's RawClassifier returned. Two bits are reserved and
    // have meaning to a source; everything above them is the consumer's own.
    std::uint32_t flags = 0;

    // A message a consumer can START from, with no history before it. A video
    // keyframe; the first row of a self-contained block.
    //
    // RESERVED BECAUSE SEEKING NEEDS IT AND NOTHING ELSE CAN SUPPLY IT. A
    // recorded source cannot serve a scrub over a stream with dependencies
    // between messages unless it knows where the independent ones are, and
    // finding out by decoding would mean the interface every panel shares
    // learning one panel's codec. So the consumer says, in one bit, and the
    // source stays ignorant of what it is saying it about.
    //
    // A stream with no dependencies -- one CAN frame, one JPEG -- sets it on
    // every message and is seekable everywhere. A stream that sets it on
    // nothing is simply not seekable, which is the honest answer rather than a
    // broken picture.
    static constexpr std::uint32_t kSeekPoint = 1u << 0;

    // State a consumer must be replayed before the seek point that follows it:
    // H.264 parameter sets, a header, a dictionary. Carried alongside the seek
    // point it precedes rather than left behind, because starting exactly at
    // the seek point would drop it.
    static constexpr std::uint32_t kPreamble = 1u << 1;

    // Where a consumer's own bits start, so adding one never collides with a
    // reserved bit that gains a meaning later.
    static constexpr std::uint32_t kFirstUserFlag = 1u << 8;
};

// How a consumer tags a message before anything has been handed to it.
//
// THIS IS WHAT KEEPS DataSource FREE OF SCHEMAS. A recorded source has to build
// a keyframe index to seek video at all, and it cannot read CarPlayVideo without
// learning about CarPlayVideo -- which would put one panel's schema into the
// interface every panel shares. Instead the source calls this and stores the
// answer, exactly as it stores a timestamp it also does not interpret.
//
// Called on the thread the message arrived on, which for a live source is a
// zenoh RX thread. It must therefore be cheap and must never block: a capnp
// header read, not a decode.
using RawClassifier = std::function<std::uint32_t(std::span<const std::uint8_t>)>;

// The retained messages, owned by the GUI thread and read with no lock during
// paint. The raw analogue of SampleHistory.
//
// A deque rather than a ring over a flat vector, because the elements own heap
// buffers of wildly different sizes -- an H.264 keyframe is two orders of
// magnitude bigger than the delta frames after it -- so the "overwrite the
// oldest slot" trick that makes SampleHistory cheap would keep the largest
// allocation alive forever and reuse none of the small ones.
class RawHistory
{
  public:
    void append(RawMessage message);

    // THE SEEK PATH. Replaces everything, so the buffer never holds messages
    // from two scrub positions at once -- see lowerBound() for why that is not
    // merely untidy.
    void replace(std::vector<RawMessage> messages);

    std::size_t size() const { return messages_.size(); }
    bool empty() const { return messages_.empty(); }
    std::size_t bytes() const { return bytes_; }

    const RawMessage& operator[](std::size_t index) const { return messages_[index]; }
    const RawMessage& oldest() const { return messages_.front(); }
    const RawMessage& newest() const { return messages_.back(); }

    // Index of the first message with t >= `t_min`, or size() when every message
    // is older. Assumes times are non-decreasing, and CANNOT DETECT OTHERWISE:
    // it is a binary search, so a buffer holding two scrub positions returns a
    // plausible wrong index rather than failing. Same precondition, and same
    // failure mode, as SampleHistory::lowerBound().
    std::size_t lowerBound(double t_min) const;

    // Trim to the retention bounds, evicting oldest. BOTH apply and whichever
    // binds first wins -- the same argument CaptureBuffer makes, for the same
    // measured reason: with CarPlay streaming the bus runs about 1.5 GB/hour, so
    // a time-only bound is an out-of-memory kill, while telemetry-sized traffic
    // would never reach a byte bound at all.
    //
    // `now` is passed in rather than read here so that a paused or seeking view
    // does not silently discard the messages it is looking at.
    void trim(double now, double history_seconds, std::size_t max_bytes);

    void clear();

  private:
    std::deque<RawMessage> messages_;
    std::size_t bytes_ = 0;
};

// A bound raw stream: the hand-off from the producer thread, plus the history
// the panel decodes from. The raw analogue of SignalBuffer, and deliberately the
// same two-stage shape.
//
// The staging half is a mutex-guarded deque rather than the lock-free
// StagingRing, and the difference is forced by the payloads. StagingRing works
// because a Sample is 16 bytes and can be copied into a preallocated slot; a
// message owning a variable-length heap buffer cannot be, so a lock-free ring
// would have to allocate on the producer thread anyway. CaptureBuffer made the
// same call for the same reason, and the mutex is only ever held for a move.
class RawBuffer
{
  public:
    // `max_bytes` bounds the staging half as well as the retained half. A single
    // 4 MB keyframe can blow a byte cap on its own, so the caller should not set
    // this anywhere near the size of one frame.
    RawBuffer(double history_seconds, std::size_t max_bytes);

    // Producer thread -- a zenoh RX thread for a live source. Never blocks for
    // longer than the mutex and never allocates beyond the move.
    //
    // Drops the NEWEST message when staging is over its byte bound, and counts
    // it, for the same reason StagingRing does: the alternative is for the
    // producer to advance an index the consumer owns. A drop here means the GUI
    // thread is wedged, and then nothing is being drawn either way.
    void push(RawMessage message);

    // GUI thread. Moves everything staged into the history and trims.
    void drain(double now);

    // GUI thread. THE SEEK PATH: replaces the retained history wholesale,
    // bypassing staging. See SignalBuffer::replaceHistory() -- the argument is
    // identical, including that the clear is what keeps lowerBound()'s
    // precondition true across a backwards seek.
    void replaceHistory(std::vector<RawMessage> messages);

    // GUI thread. Playback moving FORWARD: appends only the newly-reached tail
    // rather than rebuilding a window that has not changed.
    void append(std::vector<RawMessage> messages);

    // GUI thread. Drops everything, staged and retained.
    //
    // received() and dropped() are NOT reset: they are lifetime counters, and a
    // scrub that zeroed them would make "this stream has produced nothing" and
    // "this stream was just reloaded" look identical in the stats -- which is
    // the one place a test can tell them apart.
    void clear();

    const RawHistory& history() const { return history_; }

    // Bumped whenever the history is REPLACED rather than extended, so a
    // consumer can tell "the source moved me to a different window" from "more
    // arrived, and the oldest was trimmed".
    //
    // A consumer cannot tell those apart from the contents. The obvious
    // discriminator -- did the oldest timestamp change? -- says yes for BOTH,
    // because trimming moves it too. A live panel using it would therefore throw
    // its decoder away and re-decode the whole retention window on every tick,
    // for ever, starting the moment retention is first reached. That is invisible
    // in a short run and ruinous in a long one, which is why this is an explicit
    // signal rather than something inferred.
    std::uint64_t generation() const { return generation_; }

    std::uint64_t received() const { return received_; }
    std::uint64_t dropped() const;

    double historySeconds() const { return history_seconds_; }
    std::size_t maxBytes() const { return max_bytes_; }

    void setBounds(double history_seconds, std::size_t max_bytes);

  private:
    double history_seconds_;
    std::size_t max_bytes_;

    mutable std::mutex mutex_;
    std::deque<RawMessage> staging_;
    std::size_t staging_bytes_ = 0;
    std::uint64_t dropped_ = 0;

    RawHistory history_;
    std::uint64_t received_ = 0;
    std::uint64_t generation_ = 0;
};

// Payload plus the struct itself. Allocator overhead is deliberately not
// counted, for the reason CaptureBuffer gives: it would make the bound depend on
// the allocator rather than on the data, and the point of the cap is to stay
// inside a memory budget rather than to be exact.
std::size_t rawMessageBytes(const RawMessage& message);

}  // namespace scope

#endif  // SCOPE_RAW_BUFFER_H_
