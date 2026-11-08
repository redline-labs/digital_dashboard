#ifndef CAN_BRIDGE_TX_QUEUE_H_
#define CAN_BRIDGE_TX_QUEUE_H_

#include "helpers/can_frame.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace can_bridge
{

// Frames waiting to be sent, between the zenoh callback that receives them and
// the thread that hands them to the adapter.
//
// The split exists because a channel's send() can block: on a USB transfer,
// and on a MoTeC UTC for the device to answer earlier transmits, up to a
// second. A zenoh callback must not block, so the callback only push()es.
// Bounded, and full means the NEWEST frame is refused: the ones already queued
// keep their order, and a publisher outrunning the bus finds out through the
// dropped count rather than through a node whose memory grows without end.
class TxQueue
{
  public:
    explicit TxQueue(size_t depth) : depth_(depth) {}

    // Never blocks. False when the queue is full or closed; a full queue
    // counts the frame as dropped.
    bool push(const helpers::CanFrame& frame)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
            {
                return false;
            }
            if (frames_.size() >= depth_)
            {
                ++dropped_;
                return false;
            }
            frames_.push_back(frame);
        }
        ready_.notify_one();
        return true;
    }

    // The next frame, waiting for one. Nullopt once closed, even with frames
    // still queued: closing is shutdown, and those are discarded.
    std::optional<helpers::CanFrame> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !frames_.empty(); });
        if (closed_)
        {
            return std::nullopt;
        }
        helpers::CanFrame frame = frames_.front();
        frames_.pop_front();
        return frame;
    }

    void close()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            frames_.clear();
        }
        ready_.notify_all();
    }

    void reopen()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    size_t size() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    uint64_t dropped() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

  private:
    const size_t depth_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<helpers::CanFrame> frames_;
    uint64_t dropped_{0};
    bool closed_{false};
};

}  // namespace can_bridge

#endif  // CAN_BRIDGE_TX_QUEUE_H_
