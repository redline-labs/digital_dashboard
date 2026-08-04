// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/virtual_backend.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace can
{
namespace
{

class VirtualChannel;

// One in-process bus. Channels register themselves here so a frame sent by any
// of them reaches all the others.
class VirtualBus
{
public:
    explicit VirtualBus(std::string name)
        : name_(std::move(name))
    {
    }

    const std::string& name() const { return name_; }

    void attach(VirtualChannel* channel);
    void detach(VirtualChannel* channel);
    void broadcast(const helpers::CanFrame& frame, VirtualChannel* from);
    size_t channel_count() const;

private:
    std::string name_;
    mutable std::mutex mutex_;
    std::vector<VirtualChannel*> channels_;
};

// The buses themselves outlive any individual channel, because two channels
// opened at different times have to find the same bus.
std::mutex g_busesMutex;
std::map<std::string, std::shared_ptr<VirtualBus>> g_buses;

std::shared_ptr<VirtualBus> bus_for(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_busesMutex);
    auto it = g_buses.find(name);
    if (it != g_buses.end())
    {
        return it->second;
    }
    auto bus = std::make_shared<VirtualBus>(name);
    g_buses[name] = bus;
    return bus;
}

std::shared_ptr<VirtualBus> find_bus(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_busesMutex);
    auto it = g_buses.find(name);
    return it == g_buses.end() ? nullptr : it->second;
}

class VirtualChannel : public Channel
{
public:
    VirtualChannel(ChannelId id, std::shared_ptr<VirtualBus> bus, const OpenOptions& options)
        : id_(std::move(id))
        , description_(fmt::format("virtual bus '{}'", bus->name()))
        , bus_(std::move(bus))
        , bitrate_(options.bitrate)
        , listenOnly_(options.listenOnly)
        , queueDepth_(options.rxQueueDepth)
    {
        bus_->attach(this);
        if (options.start)
        {
            running_ = true;
            statistics_.state = BusState::ErrorActive;
        }
    }

    ~VirtualChannel() override { bus_->detach(this); }

    const ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }

    Result<void> set_bitrate(const Bitrate& bitrate) override
    {
        // Nothing here is timed, so any rate is achievable. Recording it means
        // a caller that reads it back sees what it asked for, which is what the
        // node's status topic reports.
        std::lock_guard<std::mutex> lock(mutex_);
        bitrate_ = bitrate;
        return {};
    }

    Bitrate bitrate() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return bitrate_;
    }

    bool supports_fd() const override { return true; }

    Result<void> set_listen_only(bool listenOnly) override
    {
        listenOnly_ = listenOnly;
        return {};
    }

    bool listen_only() const override { return listenOnly_; }

    Result<void> start() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        statistics_.state = BusState::ErrorActive;
        return {};
    }

    Result<void> stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            statistics_.state = BusState::Stopped;
        }
        // Wake anything blocked in receive() so a shutdown does not have to
        // wait out the timeout.
        arrived_.notify_all();
        return {};
    }

    bool running() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    Result<void> send(const helpers::CanFrame& frame) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
            {
                return invalid_state(fmt::format("{} is not running", id_.toString()));
            }
            if (listenOnly_)
            {
                return invalid_state(
                    fmt::format("{} is listen-only and cannot transmit", id_.toString()));
            }
            if (!frame.id_fits())
            {
                return invalid_argument(fmt::format(
                    "identifier 0x{:X} does not fit an {}-bit frame", frame.id,
                    frame.isExtended ? 29 : 11));
            }
            statistics_.txFrames++;
            statistics_.txBytes += frame.len;
        }

        bus_->broadcast(frame, this);
        return {};
    }

    Result<size_t> receive(std::span<helpers::CanFrame> out, Duration timeout) override
    {
        if (out.empty())
        {
            return size_t { 0 };
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

    Statistics statistics() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return statistics_;
    }

    // Called by the bus, from whichever thread sent the frame.
    void deliver(const helpers::CanFrame& frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
            {
                return;
            }
            if (queue_.size() >= queueDepth_)
            {
                // Drop the oldest. A monitoring channel that has stopped being
                // read should keep the most recent traffic, not the traffic
                // from whenever it stopped.
                queue_.pop_front();
                statistics_.rxDropped++;
            }
            queue_.push_back(frame);
            statistics_.rxFrames++;
            statistics_.rxBytes += frame.len;
        }
        arrived_.notify_one();
    }

private:
    ChannelId id_;
    std::string description_;
    std::shared_ptr<VirtualBus> bus_;

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<helpers::CanFrame> queue_;

    Bitrate bitrate_;
    bool listenOnly_ { false };
    bool running_ { false };
    size_t queueDepth_ { 8192 };
    Statistics statistics_ {};
};

void VirtualBus::attach(VirtualChannel* channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.push_back(channel);
}

void VirtualBus::detach(VirtualChannel* channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.erase(std::remove(channels_.begin(), channels_.end(), channel), channels_.end());
}

void VirtualBus::broadcast(const helpers::CanFrame& frame, VirtualChannel* from)
{
    // Copy under the lock, deliver outside it: deliver() takes the receiving
    // channel's lock, and holding the bus lock across that would order two
    // locks in a way the receive path could contend with.
    std::vector<VirtualChannel*> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = channels_;
    }

    for (VirtualChannel* channel : targets)
    {
        // A controller without loopback does not hear itself.
        if (channel != from)
        {
            channel->deliver(frame);
        }
    }
}

size_t VirtualBus::channel_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.size();
}

class VirtualBackend : public Backend
{
public:
    const std::string& name() const override { return name_; }

    std::vector<ChannelInfo> enumerate() override
    {
        // Virtual buses come into existence when they are opened, so there is
        // nothing to discover. Listing the ones that happen to be open would
        // suggest they are hardware, which they are not.
        return {};
    }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id,
                                          const OpenOptions& options) override
    {
        if (id.channel != 0)
        {
            return invalid_argument(fmt::format(
                "'{}' has channel {}; a virtual bus has one channel per name, so use a different "
                "name instead",
                id.toString(), id.channel));
        }
        return std::make_shared<VirtualChannel>(id, bus_for(id.device), options);
    }

private:
    std::string name_ { "virtual" };
};

} // namespace

std::shared_ptr<Backend> make_virtual_backend()
{
    return std::make_shared<VirtualBackend>();
}

void virtual_bus_inject(const std::string& bus, const helpers::CanFrame& frame)
{
    auto found = find_bus(bus);
    if (found != nullptr)
    {
        // No sender, so every channel on the bus receives it.
        found->broadcast(frame, nullptr);
    }
}

size_t virtual_bus_channel_count(const std::string& bus)
{
    auto found = find_bus(bus);
    return found == nullptr ? 0 : found->channel_count();
}

} // namespace can
