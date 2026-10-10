#ifndef CAN_BRIDGE_BRIDGED_CHANNEL_H_
#define CAN_BRIDGE_BRIDGED_CHANNEL_H_

#include "node_config.h"
#include "trc_recorder.h"

#include "can/backend.h"
#include "can/channel.h"

#include "can_bridge.capnp.h"
#include "can_frame.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace can_bridge
{

// How a configured channel is opened, at startup and on every reopen.
can::OpenOptions openOptions(const ChannelConfig& config);

// When to try opening a channel again: at once the first time, then after a
// delay that doubles up to a ceiling. An adapter that is gone for an hour is
// asked about every half minute, not every tick, and one that comes back is
// picked up within that.
class ReopenBackoff
{
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kFirst{1000};
    static constexpr std::chrono::milliseconds kCeiling{30000};

    bool due(Clock::time_point now) const { return now >= next_; }

    void failed(Clock::time_point now)
    {
        next_ = now + delay_;
        delay_ = std::min<std::chrono::milliseconds>(delay_ * 2, kCeiling);
    }

    void reset()
    {
        delay_ = kFirst;
        next_ = {};
    }

  private:
    std::chrono::milliseconds delay_{kFirst};
    Clock::time_point next_{};
};

// One configured bus: the hardware, the topics, and the thread pumping between
// them.
//
// The hardware can go away and come back -- an unplugged adapter -- while the
// topics and the trace being recorded stay put: reattach() swaps the channel
// under them, so a reconnect does not truncate the recording or redeclare the
// publishers.
class BridgedChannel
{
  public:
    BridgedChannel(ChannelConfig config, std::shared_ptr<can::Channel> channel);
    ~BridgedChannel();

    BridgedChannel(const BridgedChannel&) = delete;
    BridgedChannel& operator=(const BridgedChannel&) = delete;

    const ChannelConfig& config() const { return config_; }
    std::shared_ptr<can::Channel> channel() const;

    void start();
    void stop();

    // True once the hardware has gone: the channel stopped running, or a
    // receive said the device is not there. A finished trace replay is not
    // lost; it is a quiet bus.
    bool lost() const;

    // Puts a freshly opened channel under the same topics and recorder, and
    // restarts the receive loop on it.
    void reattach(std::shared_ptr<can::Channel> channel);

    ReopenBackoff& reopen() { return reopen_; }

    // What this channel is doing, for the status topic.
    void fill_status(CanBridgeChannelStatus::Builder builder) const;

  private:
    void startPump();
    void stopPump();
    void transmit(::CanFrame::Reader message);
    void pump();
    void publish(const helpers::CanFrame& frame);
    void note_error(std::string message);

    ChannelConfig config_;

    // Read by the tx subscriber's thread and the pump, replaced by reattach().
    mutable std::mutex channelMutex_;
    std::shared_ptr<can::Channel> channel_;

    std::unique_ptr<pub_sub::ZenohPublisher<::CanFrame>> rxPublisher_;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::CanFrame>> txSubscriber_;
    std::unique_ptr<TrcRecorder> recorder_;

    std::thread pump_;
    std::atomic<bool> pumping_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> deviceGone_{false};
    ReopenBackoff reopen_;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

// A channel that could not be opened. Kept rather than dropped, so the status
// topic reports what is wrong with it instead of it simply being absent -- "the
// adapter is held by the kernel driver" is a far more useful thing to publish
// than nothing at all -- and so it can be opened when the adapter turns up.
struct FailedChannel
{
    ChannelConfig config;
    std::string error;
    ReopenBackoff reopen;
};

}  // namespace can_bridge

#endif  // CAN_BRIDGE_BRIDGED_CHANNEL_H_
