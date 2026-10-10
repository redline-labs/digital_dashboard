#ifndef BYTE_STREAM_BACKOFF_H_
#define BYTE_STREAM_BACKOFF_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace byte_stream
{

// How long to wait before the next connection attempt.
//
// Each failure -- an attempt that did not connect, or a connection that ended
// -- steps one place along the schedule and stays on its last entry. Only a
// connection that STAYED UP for `stableAfter` starts it over. Resetting on
// connect instead let a peer that accepts and then closes at once be
// reconnected in a tight loop, because every attempt "succeeded".
//
// An empty schedule means no wait at all; the node configs refuse one, so
// only a test asks for it.
class Backoff
{
  public:
    using Clock = std::chrono::steady_clock;

    explicit Backoff(std::vector<std::chrono::milliseconds> schedule,
                     std::chrono::milliseconds stableAfter = std::chrono::seconds(10))
        : schedule_(std::move(schedule)), stableAfter_(stableAfter)
    {
    }

    void connected(Clock::time_point now) { connectedAt_ = now; }

    // The wait before the next attempt, after a failure or a disconnect at
    // `now`.
    std::chrono::milliseconds failed(Clock::time_point now)
    {
        if (connectedAt_ && now - *connectedAt_ >= stableAfter_)
        {
            index_ = 0;
        }
        connectedAt_.reset();

        if (schedule_.empty())
        {
            return std::chrono::milliseconds(0);
        }
        const std::chrono::milliseconds wait = schedule_[std::min(index_, schedule_.size() - 1)];
        index_ = std::min(index_ + 1, schedule_.size() - 1);
        return wait;
    }

  private:
    std::vector<std::chrono::milliseconds> schedule_;
    std::chrono::milliseconds stableAfter_;
    std::size_t index_{0};
    std::optional<Clock::time_point> connectedAt_;
};

// Sleeps for `total`, waking promptly once `keepGoing()` returns false. A five
// second backoff that could not be interrupted would make Ctrl-C take five
// seconds, long enough that people reach for SIGKILL.
template <typename KeepGoing>
void sleepWhile(std::chrono::milliseconds total, KeepGoing keepGoing)
{
    constexpr auto kSlice = std::chrono::milliseconds(50);
    while (total.count() > 0 && keepGoing())
    {
        const auto slice = std::min(kSlice, total);
        std::this_thread::sleep_for(slice);
        total -= slice;
    }
}

}  // namespace byte_stream

#endif  // BYTE_STREAM_BACKOFF_H_
