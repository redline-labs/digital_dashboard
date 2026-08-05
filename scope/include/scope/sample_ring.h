#ifndef SCOPE_SAMPLE_RING_H_
#define SCOPE_SAMPLE_RING_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace scope
{

// One point on a plot: a time in seconds since the source's epoch, and a value.
struct Sample
{
    double t = 0.0;
    double v = 0.0;
};

// The producer half: a fixed-capacity single-producer/single-consumer ring
// written from the thread samples arrive on and drained from the GUI thread.
//
// It never blocks and never allocates, because the producer is a zenoh RX
// thread. A mutex here would let a busy GUI thread stall the bus, and an
// allocation would do the same less predictably.
//
// When it is full it drops the NEWEST sample and counts the drop. Dropping the
// oldest would keep the plot's right edge live, which is what you want, but the
// producer would have to advance the read index the consumer owns, and making
// that safe needs sequence numbers and a retry loop. It is not worth it: at the
// default capacity a drop needs a sustained rate three orders of magnitude
// above anything on a CAN bus, so in practice overflow means the GUI thread is
// wedged -- and then nothing is being drawn either way.
//
// What matters is that drops are counted rather than silent. A plot that
// quietly discards samples is a plot that lies, and the count is what lets the
// panel say so.
class StagingRing
{
  public:
    explicit StagingRing(std::size_t capacity);

    // Producer thread only. False when the ring was full and this sample was
    // dropped.
    //
    // A false return counts a drop. That is right for the producer this exists
    // for -- a zenoh callback pushes once and moves on, because it may not
    // block -- but it means a caller that retries the same sample counts it
    // once per attempt. Anything that wants to wait for room should spin on
    // full() instead.
    bool push(const Sample& sample);

    // Producer thread only, and inherently a snapshot: the consumer may drain
    // immediately after. Only useful for "wait until there is room", which the
    // real producer never does.
    bool full() const;

    // Consumer thread only. Appends everything available to `out` and returns
    // how many were appended.
    std::size_t drain(std::vector<Sample>& out);

    // Either thread. Monotonic.
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    std::size_t capacity() const { return capacity_; }

  private:
    // capacity_ + 1 slots: one is always left empty so that "head == tail"
    // means empty and never ambiguously means full.
    std::vector<Sample> slots_;
    std::size_t capacity_;

    // head_ is written by the producer and read by the consumer; tail_ the
    // other way round. Each is only ever advanced by one side, which is what
    // makes this safe without a lock.
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};

    std::atomic<std::uint64_t> dropped_{0};
};

// The consumer half: the retained history, owned by the GUI thread and read
// with no lock at all during paint.
//
// A ring over a flat vector rather than a std::deque, because painting wants
// two things a deque makes awkward: a binary search for the first sample in the
// visible window, and a tight index walk from there. Logical index 0 is the
// oldest retained sample.
class SampleHistory
{
  public:
    explicit SampleHistory(std::size_t capacity);

    // Overwrites the oldest sample when full. That is the right behaviour here,
    // unlike in StagingRing: this side is single-threaded, so advancing the
    // read position is free, and a plot should keep its newest data.
    void append(const Sample& sample);

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::size_t capacity() const { return slots_.size(); }

    // `index` is logical: 0 is the oldest retained sample, size()-1 the newest.
    const Sample& operator[](std::size_t index) const
    {
        return slots_[(head_ + index) % slots_.size()];
    }

    const Sample& oldest() const { return (*this)[0]; }
    const Sample& newest() const { return (*this)[size_ - 1]; }

    // Index of the first sample with t >= `t_min`, or size() when every sample
    // is older than that. Assumes times are non-decreasing, which they are:
    // a live source stamps on arrival from one clock, and a recorded source
    // replays in order.
    std::size_t lowerBound(double t_min) const;

    // Drops everything older than `t_min`. O(number dropped).
    void trimOlderThan(double t_min);

    void clear();

  private:
    std::vector<Sample> slots_;
    std::size_t head_ = 0;  // Index of the oldest sample.
    std::size_t size_ = 0;
};

// A bound signal's data: the lock-free hand-off from the producer, plus the
// history the panel draws.
//
// Deliberately NOT modelled on dashboard::ExpressionSubscription, which is the
// established shape in this tree for getting a value from zenoh onto the GUI
// thread. That one is a single-slot mailbox: it keeps the newest sample between
// GUI ticks and discards the rest. For a gauge that is exactly right -- you
// want the current reading and nothing else. For a plot it is fatal, because
// the samples it throws away are the line.
class SignalBuffer
{
  public:
    // `history_seconds` and `max_points` are both caps: whichever binds first
    // wins, so a fast publisher cannot grow this without bound and a slow one
    // still gets its full time window.
    SignalBuffer(double history_seconds, std::size_t max_points, std::size_t staging_capacity);

    // Producer thread. Never blocks.
    void push(const Sample& sample);

    // GUI thread. Moves everything staged into the history and trims to the
    // retention limits. `now` is the current time on the source's clock, and is
    // what the time-based trim measures back from -- passed in rather than read
    // here so that a paused or seeking view does not silently discard the data
    // it is looking at.
    void drain(double now);

    const SampleHistory& history() const { return history_; }

    std::uint64_t dropped() const { return staging_.dropped(); }

    // Total samples that reached the history since construction, which is what
    // "is this signal actually producing anything" wants.
    std::uint64_t received() const { return received_; }

    double historySeconds() const { return history_seconds_; }

  private:
    double history_seconds_;
    StagingRing staging_;
    SampleHistory history_;
    std::uint64_t received_ = 0;

    // Scratch for drain(), kept so a 30 Hz drain does not allocate.
    std::vector<Sample> scratch_;
};

}  // namespace scope

#endif  // SCOPE_SAMPLE_RING_H_
