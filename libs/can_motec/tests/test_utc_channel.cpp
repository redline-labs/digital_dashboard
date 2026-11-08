// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MoTeC channel's session behaviour, against a fake gateway on loopback
// UDP -- the same envelope a UTC speaks over USB, with no dongle needed.
//
// What it pins, each of which was wrong once and failed silently:
//   - the bit rate travels in byte +11 of the Mode command. That byte was
//     taken for padding and sent as 0, which set every UTC to 1 Mbit/s while
//     the backend said it could not set a rate at all;
//   - changing the rate is a new session (Open, Version, Mode, subscribe);
//   - no more than two transmits are left unanswered on the wire -- flooding a
//     real UTC latched it until power-cycled;
//   - a record with no format bit is damage, dropped and counted, not a frame
//     on identifier 0;
//   - a link that fails ends the channel instead of being logged forever.

#include "can_motec/motec_gw.h"
#include "can_motec/utc_backend.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using namespace can::motec;

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

// Answers Open, Version and Mode as a UTC does, records every request, and
// acknowledges transmits only while told to.
class FakeGateway
{
public:
    FakeGateway()
    {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeGateway() { close(); }

    void close()
    {
        if (thread_.joinable())
        {
            serving_ = false;
            thread_.join();
            ::close(fd_);
        }
    }

    uint16_t port() const { return port_; }

    std::vector<Frame> received()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

    size_t count(Code code)
    {
        size_t n = 0;
        for (const auto& frame : received())
        {
            n += frame.code() == code ? 1u : 0u;
        }
        return n;
    }

    // While held, transmits are recorded but not answered.
    void hold_tx(bool hold)
    {
        std::vector<Frame> release;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            holdTx_ = hold;
            if (!hold)
            {
                release.swap(heldTx_);
            }
        }
        for (const auto& tx : release)
        {
            answer_tx(tx);
        }
    }

    // One unsolicited Rx data frame carrying `records`.
    void push_rx(const std::vector<Record>& records)
    {
        Frame frame;
        frame.cmd = static_cast<uint8_t>((kDefaultTag << 5) | static_cast<uint8_t>(Code::Rx)
                                         | kReplyBit);
        frame.field5 = kHandle;
        frame.reqid = 0x40;
        frame.data = pack_records(records);
        frame.payload = { 0, static_cast<uint8_t>(frame.data.size() >> 8),
                          static_cast<uint8_t>(frame.data.size() & 0xFF) };
        send(frame);
    }

private:
    static constexpr uint8_t kHandle = 0x01;

    static Frame reply_to(const Frame& request, std::vector<uint8_t> payload)
    {
        Frame reply;
        reply.cmd = static_cast<uint8_t>(request.cmd | kReplyBit);
        reply.field5 = request.code() == Code::Open ? kHandle : request.field5;
        reply.reqid = request.reqid;
        reply.payload = std::move(payload);
        return reply;
    }

    void answer_tx(const Frame& tx)
    {
        send(reply_to(tx, { 0, 0, static_cast<uint8_t>(kRecordSize) }));
    }

    void send(const Frame& frame)
    {
        const auto bytes = encode_frame(frame);
        std::lock_guard<std::mutex> lock(peerMutex_);
        if (peerLength_ != 0)
        {
            ::sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&peer_),
                     peerLength_);
        }
    }

    void serve()
    {
        while (serving_)
        {
            pollfd poller { fd_, POLLIN, 0 };
            if (::poll(&poller, 1, 20) <= 0)
            {
                continue;
            }
            std::array<uint8_t, 2048> buffer {};
            sockaddr_storage from {};
            socklen_t fromLength = sizeof(from);
            const ssize_t got = ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                                           reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (got <= 0)
            {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(peerMutex_);
                peer_ = from;
                peerLength_ = fromLength;
            }

            auto frame = decode_frame(std::span(buffer.data(), static_cast<size_t>(got)));
            if (!frame.has_value())
            {
                continue;
            }

            bool hold = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                received_.push_back(*frame);
                if (frame->code() == Code::Tx && holdTx_)
                {
                    heldTx_.push_back(*frame);
                    hold = true;
                }
            }

            switch (frame->code())
            {
            case Code::Open:
            case Code::Mode:
                send(reply_to(*frame, { 0 }));
                break;
            case Code::Version:
                send(reply_to(*frame, { 0, 7, 2 }));
                break;
            case Code::Tx:
                if (!hold)
                {
                    answer_tx(*frame);
                }
                break;
            case Code::Rx:
            case Code::Close:
            case Code::Ack:
            case Code::RegRead:
            case Code::Set:
                break;
            }
        }
    }

    int fd_ { -1 };
    uint16_t port_ { 0 };
    std::atomic<bool> serving_ { true };
    std::thread thread_;

    std::mutex peerMutex_;
    sockaddr_storage peer_ {};
    socklen_t peerLength_ { 0 };

    std::mutex mutex_;
    std::vector<Frame> received_;
    bool holdTx_ { false };
    std::vector<Frame> heldTx_;
};

MotecOptions fast_options()
{
    MotecOptions options;
    options.txAnswerTimeoutMs = 300;
    return options;
}

can::Result<std::shared_ptr<can::Channel>> open_at(FakeGateway& gateway, uint32_t bps,
                                                   const MotecOptions& options = fast_options())
{
    can::OpenOptions open;
    open.bitrate.nominalBps = bps;
    return make_motec_backend(options)->open(
        can::ChannelId { "motec", fmt::format("udp=127.0.0.1:{}", gateway.port()), 0 }, open);
}

std::vector<Frame> of_code(const std::vector<Frame>& frames, Code code)
{
    std::vector<Frame> out;
    for (const auto& frame : frames)
    {
        if (frame.code() == code)
        {
            out.push_back(frame);
        }
    }
    return out;
}

void test_the_bit_rate_reaches_the_mode_command()
{
    FakeGateway gateway;
    auto channel = open_at(gateway, 500000);
    check(channel.has_value(), "a channel opens at 500 kbit/s");
    if (!channel.has_value())
    {
        return;
    }

    const auto modes = of_code(gateway.received(), Code::Mode);
    check(modes.size() == 1, fmt::format("one Mode per session, got {}", modes.size()));
    if (modes.size() == 1 && modes[0].payload.size() == 12)
    {
        check(modes[0].payload[8] == kModeStream, "in stream mode");
        check(modes[0].payload[11] == 1,
              fmt::format("500 kbit/s is rate index 1, the Mode carried {}", modes[0].payload[11]));
    }
    check((*channel)->bitrate().nominalBps == 500000, "bitrate() is what the device was told");
}

void test_a_rate_the_device_cannot_run_is_refused_before_anything_is_sent()
{
    FakeGateway gateway;
    for (const uint32_t bps : { 800000u, 0u, 2000000u })
    {
        auto channel = open_at(gateway, bps);
        check(!channel.has_value(), fmt::format("{} bit/s is refused", bps));
        if (!channel.has_value())
        {
            check(channel.error().kind == can::Error::Kind::Unsupported,
                  fmt::format("{} bit/s is refused as Unsupported", bps));
        }
    }
    std::this_thread::sleep_for(50ms);
    check(gateway.received().empty(), "and nothing reached the device");
}

void test_changing_the_rate_is_a_new_session()
{
    FakeGateway gateway;
    auto channel = open_at(gateway, 1000000);
    check(channel.has_value(), "a channel opens at 1 Mbit/s");
    if (!channel.has_value())
    {
        return;
    }

    can::Bitrate slower;
    slower.nominalBps = 250000;
    check((*channel)->set_bitrate(slower).has_value(), "set_bitrate to 250 kbit/s succeeds");
    check((*channel)->running(), "and the channel is running again after it");

    std::this_thread::sleep_for(50ms);
    const auto frames = gateway.received();
    const auto modes = of_code(frames, Code::Mode);
    check(of_code(frames, Code::Open).size() == 2, "the change opened a second session");
    check(modes.size() == 2 && modes.back().payload.size() == 12 && modes.back().payload[11] == 2,
          "whose Mode carried index 2, 250 kbit/s");
    check(of_code(frames, Code::Rx).size() == 2, "and the stream was subscribed again");

    can::Bitrate impossible;
    impossible.nominalBps = 800000;
    check(!(*channel)->set_bitrate(impossible).has_value(), "800 kbit/s is refused");
    check(of_code(gateway.received(), Code::Open).size() == 2,
          "without disturbing the running session");
}

void test_damaged_records_are_dropped_and_counted()
{
    FakeGateway gateway;
    auto channel = open_at(gateway, 1000000);
    check(channel.has_value(), "a channel opens");
    if (!channel.has_value())
    {
        return;
    }

    Record standard;
    standard.wireId = kStandardIdBit | (0x123u << kStandardIdShift);
    standard.flags = 2;
    Record damaged; // the identifier word zeroed, the data intact
    damaged.flags = 8;
    damaged.data = { 1, 2, 3, 4, 5, 6, 7, 8 };
    Record extended;
    extended.wireId = kExtendedIdBit | 0x1CF81BECu;
    extended.flags = 8;
    gateway.push_rx({ standard, damaged, extended });

    std::vector<helpers::CanFrame> got;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (got.size() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        std::array<helpers::CanFrame, 8> out {};
        auto count = (*channel)->receive(out, 100ms);
        if (count.has_value())
        {
            got.insert(got.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(*count));
        }
    }
    std::array<helpers::CanFrame, 8> more {};
    auto extra = (*channel)->receive(more, 100ms);

    check(got.size() == 2 && extra.has_value() && *extra == 0,
          fmt::format("two frames delivered, got {}", got.size() + extra.value_or(0)));
    if (got.size() == 2)
    {
        check(got[0].id == 0x123 && !got[0].isExtended, "the standard frame");
        check(got[1].id == 0x1CF81BEC && got[1].isExtended, "the extended frame");
    }
    const auto stats = (*channel)->statistics();
    check(stats.rxDropped == 1, fmt::format("the damaged record is counted, rxDropped {}",
                                            stats.rxDropped));
    check(stats.rxFrames == 2, "and not counted as received");
}

void test_at_most_two_transmits_are_left_unanswered()
{
    FakeGateway gateway;
    auto opened = open_at(gateway, 1000000);
    check(opened.has_value(), "a channel opens");
    if (!opened.has_value())
    {
        return;
    }
    auto channel = *opened;

    helpers::CanFrame frame {};
    frame.id = 0x100;
    frame.len = 1;

    gateway.hold_tx(true);
    check(channel->send(frame).has_value(), "the first transmit goes");
    check(channel->send(frame).has_value(), "the second transmit goes");

    // The third waits for an answer. Release one and it goes.
    auto third = std::async(std::launch::async, [&] { return channel->send(frame); });
    std::this_thread::sleep_for(100ms);
    check(gateway.count(Code::Tx) == 2,
          fmt::format("two unanswered transmits on the wire, not {}", gateway.count(Code::Tx)));
    gateway.hold_tx(false);
    check(third.get().has_value(), "the third goes once an earlier one is answered");

    // A device that stops answering is reported, and the frame not written.
    std::this_thread::sleep_for(50ms);
    gateway.hold_tx(true);
    channel->send(frame);
    channel->send(frame);
    // Both on the wire before counting: UDP to the fake is not synchronous.
    const auto settle = std::chrono::steady_clock::now() + 1s;
    while (gateway.count(Code::Tx) < 5 && std::chrono::steady_clock::now() < settle)
    {
        std::this_thread::sleep_for(5ms);
    }
    const size_t before = gateway.count(Code::Tx);
    check(before == 5, fmt::format("five transmits reached the device, got {}", before));
    const auto started = std::chrono::steady_clock::now();
    auto stuck = channel->send(frame);
    const auto waited = std::chrono::steady_clock::now() - started;
    check(!stuck.has_value(), "a transmit with no room after the timeout fails");
    check(waited >= 250ms, "after waiting for the answer timeout");
    check(gateway.count(Code::Tx) == before, "and was never written");
    gateway.hold_tx(false);
}

void test_a_failed_link_ends_the_channel()
{
    FakeGateway gateway;
    MotecOptions options = fast_options();
    // The keep-alive is what provokes the ICMP port-unreachable that a
    // connected UDP socket then reports.
    options.keepAliveIntervalMs = 50;
    auto opened = open_at(gateway, 1000000, options);
    check(opened.has_value(), "a channel opens");
    if (!opened.has_value())
    {
        return;
    }
    auto channel = *opened;
    gateway.close();

    bool ended = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!ended && std::chrono::steady_clock::now() < deadline)
    {
        std::array<helpers::CanFrame, 4> out {};
        auto got = channel->receive(out, 100ms);
        ended = !got.has_value();
        if (ended)
        {
            check(got.error().kind == can::Error::Kind::NotFound,
                  "a dead link answers receive() with NotFound");
        }
    }
    check(ended, "receive() reports the dead link");
    check(!channel->running(), "and the channel stops reporting itself running");

    helpers::CanFrame frame {};
    frame.id = 0x100;
    check(!channel->send(frame).has_value(), "and send() refuses");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_bit_rate_reaches_the_mode_command();
    test_a_rate_the_device_cannot_run_is_refused_before_anything_is_sent();
    test_changing_the_rate_is_a_new_session();
    test_damaged_records_are_dropped_and_counted();
    test_at_most_two_transmits_are_left_unanswered();
    test_a_failed_link_ends_the_channel();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }
    SPDLOG_WARN("all MoTeC channel checks passed");
    return 0;
}
