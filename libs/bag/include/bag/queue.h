#ifndef BAG_QUEUE_H_
#define BAG_QUEUE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bag
{

// One message on its way from a zenoh callback to the writer thread.
//
// Everything is owned rather than viewed. The views a zenoh callback is handed
// are valid only for the duration of the call, and the writer thread reads them
// later by definition -- so the copy is not avoidable, it is the point.
struct QueuedMessage
{
    std::string key;
    std::string schema;
    std::string origin_zid;
    std::vector<std::uint8_t> payload;
    std::uint64_t log_time_ns = 0;
    std::optional<std::uint64_t> publish_time_ns;
};

// A bounded queue between the zenoh callbacks and the writer thread.
//
// WHY BOUNDED. A zenoh callback must not block: it runs on a zenoh RX thread,
// and stalling there stalls the session for everything, including the
// liveliness traffic other tools depend on. So the recorder cannot apply
// backpressure -- when the disk cannot keep up, the only choices are to grow
// without limit until the process is killed, or to drop.
//
// Dropping, and COUNTING. An unbounded queue turns a slow disk into an
// out-of-memory kill partway through a recording, which loses everything
// including the evidence of why. A bounded one loses the samples it could not
// write and says exactly how many, which lands in metadata.yaml and in
// `bag info`. A gap that is reported is a different thing entirely from a gap
// that is not: the first is a recorder problem, the second reads as a publisher
// that stopped.
//
// Dropping the OLDEST rather than the newest, deliberately: when a recorder
// falls behind, the freshest samples are the ones most likely to explain what is
// happening now.
class MessageQueue
{
  public:
    explicit MessageQueue(std::size_t capacity) : capacity_(capacity) {}

    // Never blocks. Returns false if a message had to be dropped to make room.
    bool push(QueuedMessage message)
    {
        bool dropped = false;
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            while (queue_.size() >= capacity_)
            {
                queue_.pop_front();
                ++dropped_;
                dropped = true;
            }
            queue_.push_back(std::move(message));
        }
        ready_.notify_one();
        return !dropped;
    }

    // Blocks until a message is available or stop() is called. Returns nullopt
    // only when stopped AND drained -- so a writer loop that runs until this
    // returns nullopt cannot lose a message that was already queued when the
    // recording was told to stop.
    std::optional<QueuedMessage> pop()
    {
        std::unique_lock<std::mutex> guard(mutex_);
        ready_.wait(guard, [this] { return !queue_.empty() || stopped_; });

        if (queue_.empty())
        {
            return std::nullopt;
        }

        QueuedMessage message = std::move(queue_.front());
        queue_.pop_front();
        return message;
    }

    void stop()
    {
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            stopped_ = true;
        }
        ready_.notify_all();
    }

    std::uint64_t dropped() const
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        return dropped_;
    }

    std::size_t depth() const
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedMessage> queue_;
    std::size_t capacity_;
    std::uint64_t dropped_ = 0;
    bool stopped_ = false;
};

}  // namespace bag

#endif  // BAG_QUEUE_H_
