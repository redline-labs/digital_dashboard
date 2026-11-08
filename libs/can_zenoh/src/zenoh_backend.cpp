// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_zenoh/zenoh_backend.h"

#include "can_frame.capnp.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/topic_key.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include <spdlog/fmt/fmt.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

namespace can_zenoh
{

namespace
{

const std::string kName = "zenoh";

class ZenohChannel : public can::Channel
{
  public:
    ZenohChannel(can::ChannelId id, const std::string& prefix, const can::OpenOptions& options)
        : id_(std::move(id))
        , description_(fmt::format("zenoh '{}/rx', '{}/tx'", prefix, prefix))
        , listenOnly_(options.listenOnly)
        , queueDepth_(options.rxQueueDepth == 0 ? 1 : options.rxQueueDepth)
        , publisher_(std::make_unique<pub_sub::ZenohPublisher<::CanFrame>>(prefix + "/tx"))
    {
        if (options.start)
        {
            running_ = true;
            statistics_.state = can::BusState::ErrorActive;
        }
        // Last: its callback uses everything above, and may run at once.
        subscriber_ = std::make_unique<pub_sub::ZenohTypedSubscriber<::CanFrame>>(
            prefix + "/rx", [this](::CanFrame::Reader message) { deliver(pub_sub::fromCapnp(message)); });
    }

    ~ZenohChannel() override
    {
        // The subscriber first: undeclaring it joins any callback still
        // delivering into the queue below.
        subscriber_.reset();
    }

    const can::ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }

    can::Result<void> set_bitrate(const can::Bitrate&) override
    {
        return can::unsupported(
            fmt::format("{} is a remote bus; its bit rate belongs to whoever owns the adapter", id_.toString()));
    }

    can::Bitrate bitrate() const override { return {}; }

    // Whatever the far end carries. A bridge on a classic adapter refuses an FD
    // frame there, not here.
    bool supports_fd() const override { return true; }

    can::Result<void> set_listen_only(bool listenOnly) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        listenOnly_ = listenOnly;
        return {};
    }

    bool listen_only() const override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return listenOnly_;
    }

    can::Result<void> start() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        statistics_.state = can::BusState::ErrorActive;
        return {};
    }

    can::Result<void> stop() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            statistics_.state = can::BusState::Stopped;
        }
        // Wake a blocked receive() so a shutdown does not wait out its timeout.
        arrived_.notify_all();
        return {};
    }

    bool running() const override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    can::Result<void> send(const helpers::CanFrame& frame) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
        {
            return can::invalid_state(fmt::format("{} is not running", id_.toString()));
        }
        if (listenOnly_)
        {
            return can::invalid_state(fmt::format("{} is listen-only and cannot transmit", id_.toString()));
        }
        if (!frame.id_fits())
        {
            return can::invalid_argument(fmt::format("identifier 0x{:X} does not fit an {}-bit frame", frame.id,
                                                     frame.isExtended ? 29 : 11));
        }
        if (frame.len > frame.data.size())
        {
            return can::invalid_argument(fmt::format("length {} is more than a frame holds", frame.len));
        }
        // Under the lock: the publisher's builder is one message, reused.
        auto& fields = publisher_->fields();
        pub_sub::toCapnp(frame, fields);
        fields.setChannel(id_.toString());
        publisher_->put();
        statistics_.txFrames++;
        statistics_.txBytes += frame.len;
        return {};
    }

    can::Result<size_t> receive(std::span<helpers::CanFrame> out, can::Duration timeout) override
    {
        if (out.empty())
        {
            return size_t{0};
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty())
        {
            arrived_.wait_for(lock, timeout, [this] { return !queue_.empty() || !running_; });
        }
        size_t count = 0;
        while (count < out.size() && !queue_.empty())
        {
            out[count++] = queue_.front();
            queue_.pop_front();
        }
        return count;
    }

    can::Statistics statistics() const override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return statistics_;
    }

  private:
    // On the subscriber's thread.
    void deliver(const helpers::CanFrame& frame)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
            {
                return;
            }
            if (queue_.size() >= queueDepth_)
            {
                // Drop the oldest, as every backend here does: a reader that
                // fell behind wants the bus as it is now.
                queue_.pop_front();
                statistics_.rxDropped++;
            }
            queue_.push_back(frame);
            statistics_.rxFrames++;
            statistics_.rxBytes += frame.len;
        }
        arrived_.notify_one();
    }

    can::ChannelId id_;
    std::string description_;

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<helpers::CanFrame> queue_;
    bool listenOnly_{false};
    bool running_{false};
    size_t queueDepth_;
    can::Statistics statistics_{};

    // Held by pointer: its destructor may throw, and a can::Channel's may not.
    std::unique_ptr<pub_sub::ZenohPublisher<::CanFrame>> publisher_;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::CanFrame>> subscriber_;
};

class ZenohBackend : public can::Backend
{
  public:
    const std::string& name() const override { return kName; }

    // A remote bus exists only once something names it.
    std::vector<can::ChannelInfo> enumerate() override { return {}; }

    can::Result<std::shared_ptr<can::Channel>> open(const can::ChannelId& id,
                                                    const can::OpenOptions& options) override
    {
        if (id.channel != 0)
        {
            return can::invalid_argument(fmt::format(
                "'{}': a zenoh: prefix cannot end in an all-digit segment, which reads as a channel number",
                id.toString()));
        }
        if (id.device.empty())
        {
            return can::invalid_argument("zenoh: needs a key prefix, as in zenoh:vehicle/can0");
        }
        if (const std::string problem = pub_sub::topicKeyProblem(id.device + "/rx"); !problem.empty())
        {
            return can::invalid_argument(fmt::format("'{}': {}", id.device, problem));
        }
        return std::shared_ptr<can::Channel>(std::make_shared<ZenohChannel>(id, id.device, options));
    }
};

}  // namespace

std::shared_ptr<can::Backend> make_zenoh_backend()
{
    return std::make_shared<ZenohBackend>();
}

}  // namespace can_zenoh
