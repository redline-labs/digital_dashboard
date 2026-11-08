#include "pdm32/top/sim_channel.h"

#include <spdlog/fmt/fmt.h>

namespace sim::sys {

namespace {

can::ChannelId channel_id_of(const std::string& device) {
    if (auto parsed = can::parse_channel_id(device)) {
        return *parsed;
    }
    return can::ChannelId{.backend = "sim", .device = device, .channel = 0};
}

}  // namespace

SimCanChannel::SimCanChannel(const std::string& device, size_t queue_depth)
    : id_(channel_id_of(device)),
      description_(fmt::format("simulated device '{}'", device)),
      queue_depth_(queue_depth == 0 ? 1 : queue_depth) {
    statistics_.state = can::BusState::ErrorActive;
}

void SimCanChannel::deliver(const helpers::CanFrame& frame, uint64_t sim_us) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        if (inbound_.size() >= queue_depth_) {
            // Drop the oldest, as every backend here does: a reader that fell behind wants the bus as it is now.
            inbound_.pop_front();
            statistics_.rxDropped++;
        }
        helpers::CanFrame stamped = frame;
        stamped.timestampUs = sim_us;
        inbound_.push_back(stamped);
        statistics_.rxFrames++;
        statistics_.rxBytes += frame.len;
    }
    arrived_.notify_one();
}

std::vector<helpers::CanFrame> SimCanChannel::take_outbound() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<helpers::CanFrame> out(outbound_.begin(), outbound_.end());
    outbound_.clear();
    return out;
}

void SimCanChannel::set_device_bitrate(uint32_t bps) {
    const std::lock_guard<std::mutex> lock(mutex_);
    device_bps_ = bps;
}

can::Result<void> SimCanChannel::set_bitrate(const can::Bitrate&) {
    return can::unsupported(
        fmt::format("{}: the device's firmware programs its own bit rate; it cannot be set from outside",
                    id_.toString()));
}

can::Bitrate SimCanChannel::bitrate() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    can::Bitrate bitrate;
    bitrate.nominalBps = device_bps_;
    return bitrate;
}

can::Result<void> SimCanChannel::set_listen_only(bool listen_only) {
    const std::lock_guard<std::mutex> lock(mutex_);
    listen_only_ = listen_only;
    return {};
}

bool SimCanChannel::listen_only() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return listen_only_;
}

can::Result<void> SimCanChannel::start() {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    statistics_.state = can::BusState::ErrorActive;
    return {};
}

can::Result<void> SimCanChannel::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        statistics_.state = can::BusState::Stopped;
    }
    // Wake a blocked receive() so a shutdown does not wait out its timeout.
    arrived_.notify_all();
    return {};
}

bool SimCanChannel::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

can::Result<void> SimCanChannel::send(const helpers::CanFrame& frame) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return can::invalid_state(fmt::format("{} is not running", id_.toString()));
    }
    if (listen_only_) {
        return can::invalid_state(fmt::format("{} is listen-only and cannot transmit", id_.toString()));
    }
    if (frame.isFD || frame.len > 8) {
        return can::invalid_argument(fmt::format("{} is classic CAN: no FD frames, at most 8 bytes", id_.toString()));
    }
    if (!frame.id_fits()) {
        return can::invalid_argument(fmt::format("identifier 0x{:X} does not fit an {}-bit frame", frame.id,
                                                 frame.isExtended ? 29 : 11));
    }
    if (outbound_.size() >= queue_depth_) {
        // The simulation is not taking frames (stopped in a breakpoint, or paused): refuse rather than grow.
        statistics_.txDropped++;
        return can::invalid_state(fmt::format("{}: the device is not taking frames", id_.toString()));
    }
    outbound_.push_back(frame);
    statistics_.txFrames++;
    statistics_.txBytes += frame.len;
    return {};
}

can::Result<size_t> SimCanChannel::receive(std::span<helpers::CanFrame> out, can::Duration timeout) {
    if (out.empty()) {
        return size_t{0};
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (inbound_.empty()) {
        arrived_.wait_for(lock, timeout, [this] { return !inbound_.empty() || !running_; });
    }
    size_t count = 0;
    while (count < out.size() && !inbound_.empty()) {
        out[count++] = inbound_.front();
        inbound_.pop_front();
    }
    return count;
}

can::Statistics SimCanChannel::statistics() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

}  // namespace sim::sys
