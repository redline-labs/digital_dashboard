// The tx topic's callback does not wait for the adapter.
//
// can::Channel::send() can block: on a USB transfer, and on a MoTeC UTC for up
// to a second while it waits for answers to earlier transmits. It used to be
// called from the zenoh callback, so a slow adapter stalled the subscriber.
// Here a channel's send() blocks until released; frames published meanwhile
// must still be taken off the topic and queued, then sent in order.

#include "can_zenoh/bridged_channel.h"

#include "pub_sub/can_frame.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"

#include <capnp/message.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A channel whose send() blocks until the gate opens.
class GatedChannel : public can::Channel
{
  public:
    const can::ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }
    can::Result<void> set_bitrate(const can::Bitrate&) override { return {}; }
    can::Bitrate bitrate() const override { return {}; }
    bool supports_fd() const override { return false; }
    can::Result<void> set_listen_only(bool) override { return {}; }
    bool listen_only() const override { return false; }
    can::Result<void> start() override { return {}; }
    can::Result<void> stop() override { return {}; }
    bool running() const override { return true; }
    can::Statistics statistics() const override { return {}; }

    can::Result<size_t> receive(std::span<helpers::CanFrame>, can::Duration timeout) override
    {
        std::this_thread::sleep_for(timeout);
        return size_t{0};
    }

    can::Result<void> send(const helpers::CanFrame& frame) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
        sent_.push_back(frame.id);
        changed_.notify_all();
        return {};
    }

    bool wait_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void open_gate()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

    std::vector<uint32_t> wait_sent(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return sent_.size() >= count; });
        return sent_;
    }

  private:
    can::ChannelId id_{"gated", "0", 0};
    std::string description_{"gated test channel"};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool open_{false};
    std::vector<uint32_t> sent_;
};

uint32_t queued(const can_zenoh::BridgedChannel& bridged)
{
    capnp::MallocMessageBuilder message;
    auto status = message.initRoot<CanBridgeChannelStatus>();
    bridged.fill_status(status);
    return status.getTxQueued();
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    can_zenoh::BridgeConfig config;
    config.name = "test";
    config.device = "gated:0";
    config.txKey = "test/can_zenoh/transmit/" + std::to_string(::getpid());
    config.publishRx = false;
    config.acceptTx = true;

    auto channel = std::make_shared<GatedChannel>();
    can_zenoh::BridgedChannel bridged(config, channel);
    bridged.start();

    pub_sub::ZenohPublisher<::CanFrame> publisher(config.txKey);
    std::this_thread::sleep_for(300ms);

    // On their own thread: zenoh can deliver a local put synchronously, so
    // with a blocking callback it is the publisher that would be stuck.
    constexpr uint32_t kFrames = 5;
    std::thread publishing([&] {
        for (uint32_t id = 1; id <= kFrames; ++id)
        {
            helpers::CanFrame frame;
            frame.id = 0x100 + id;
            frame.len = 1;
            auto& fields = publisher.fields();
            pub_sub::toCapnp(frame, fields);
            publisher.put();
        }
    });

    expect(channel->wait_entered(2s), "the first frame reaches send(), which blocks");

    // With send() blocked, the callback must still take every later frame.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (queued(bridged) < kFrames - 1 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    expect(queued(bridged) == kFrames - 1,
           "the other frames were queued while send() was blocked, got "
               + std::to_string(queued(bridged)));

    channel->open_gate();
    publishing.join();
    const auto sent = channel->wait_sent(kFrames, 2s);
    expect(sent.size() == kFrames, "every frame is sent once the adapter frees up");
    for (size_t i = 0; i < sent.size(); ++i)
    {
        expect(sent[i] == 0x101 + i, "in the order published");
    }
    expect(queued(bridged) == 0, "and the queue empties");

    bridged.stop();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
