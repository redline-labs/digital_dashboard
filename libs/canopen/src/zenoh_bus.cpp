// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/zenoh_bus.h"

#include "can_frame.capnp.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>

namespace canopen
{

// The zenoh types are kept out of the header so that including
// canopen/zenoh_bus.h does not drag capnp and zenoh into every translation
// unit that merely wants to hold a pointer to one.
struct ZenohBus::Impl
{
    explicit Impl(const std::string& txKey)
        : publisher(txKey)
    {
    }

    pub_sub::ZenohPublisher<::CanFrame> publisher;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::CanFrame>> subscriber;
};

ZenohBus::ZenohBus(std::string txKey, std::string rxKey)
    : txKey_(std::move(txKey))
    , rxKey_(std::move(rxKey))
    , impl_(std::make_unique<Impl>(txKey_))
{
    impl_->subscriber = std::make_unique<pub_sub::ZenohTypedSubscriber<::CanFrame>>(
        rxKey_,
        [this](::CanFrame::Reader message)
        {
            helpers::CanFrame frame {};
            frame.id = message.getId();
            frame.len = static_cast<uint8_t>(message.getLen());

            auto data = message.getData();
            const size_t n = std::min<size_t>(
                frame.data.size(), std::min<size_t>(frame.len, data.size()));
            for (size_t i = 0; i < n; ++i)
            {
                frame.data[i] = static_cast<uint8_t>(data[i]);
            }

            // This runs on a zenoh thread. Queue it and let poll() hand it to
            // the protocol code on the thread that is waiting.
            std::lock_guard<std::mutex> lock(mutex_);
            if (received_.size() >= kMaxQueued)
            {
                ++dropped_;
                return;
            }
            received_.push_back(frame);
        });
}

ZenohBus::~ZenohBus() = default;

bool ZenohBus::is_valid() const
{
    return impl_ != nullptr && impl_->publisher.isValid();
}

uint64_t ZenohBus::dropped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void ZenohBus::send(const helpers::CanFrame& frame)
{
    auto& fields = impl_->publisher.fields();
    fields.setId(frame.id);
    fields.setLen(frame.len);

    const size_t n = std::min<size_t>(frame.data.size(), frame.len);
    auto data = fields.initData(static_cast<unsigned>(n));
    for (size_t i = 0; i < n; ++i)
    {
        data.set(static_cast<unsigned>(i), frame.data[i]);
    }

    impl_->publisher.put();
}

void ZenohBus::poll(Duration budget)
{
    // Drain whatever is already queued first, so a burst of frames is
    // delivered without waiting for the budget to elapse.
    std::deque<helpers::CanFrame> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(received_);
    }

    if (batch.empty())
    {
        // Nothing to do but let the zenoh thread get some. Sleeping the whole
        // budget is right here: the caller's wait_until() slices it finely
        // enough that a frame arriving early is picked up on the next slice.
        std::this_thread::sleep_for(budget);

        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(received_);
    }

    for (const auto& frame : batch)
    {
        deliver(frame);
    }
}

Clock::time_point ZenohBus::now() const
{
    return Clock::now();
}

} // namespace canopen
