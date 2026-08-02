// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/timingServer.ts
#include "airplay/timing.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace airplay
{
namespace
{

// RTCP-style payload types used by the CarPlay timing channel.
constexpr uint8_t kPayloadRequest = 210;
constexpr uint8_t kPayloadResponse = 211;
constexpr size_t kPacketSize = 32;
constexpr auto kRequestInterval = std::chrono::milliseconds(1000);

void writeNtpImpl(uint8_t* buffer, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        buffer[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xFF);
    }
}

uint64_t readNtpImpl(const uint8_t* buffer)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value = (value << 8) | buffer[i];
    }
    return value;
}

}  // namespace

namespace ntp
{

void write(uint8_t* buffer, uint64_t value)
{
    writeNtpImpl(buffer, value);
}

uint64_t read(const uint8_t* buffer)
{
    return readNtpImpl(buffer);
}

int64_t toNanos(uint64_t ntp)
{
    // Seconds in the high 32 bits (up to ~4.29e9, so ~4.29e18 ns -- inside
    // int64) and a 2^-32 second fraction in the low.
    const int64_t seconds = static_cast<int64_t>(ntp >> 32);
    const int64_t fraction = static_cast<int64_t>(ntp & 0xFFFFFFFFull);
    return seconds * 1000000000LL + ((fraction * 1000000000LL) >> 32);
}

double offsetSeconds(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4)
{
    constexpr double kTwo32 = 4294967296.0;
    return 0.5 *
           (static_cast<double>(static_cast<int64_t>(t2 - t1)) +
            static_cast<double>(static_cast<int64_t>(t3 - t4))) /
           kTwo32;
}

}  // namespace ntp

TimingSync::~TimingSync()
{
    stop();
}

int64_t TimingSync::rawNowNs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

uint64_t TimingSync::syncedNtp() const
{
    int64_t nanos = rawNowNs() + clock_offset_ns_;
    if (nanos < 0)
    {
        // Only reachable if the offset is nonsense; casting a negative to
        // uint64 below would wrap to ~1.8e19 ns and the seconds would then
        // shift straight out of the top of the timestamp. Report the epoch
        // rather than a number the phone will act on.
        nanos = 0;
    }
    const uint64_t seconds = static_cast<uint64_t>(nanos) / 1000000000ULL;
    const uint64_t fraction =
        ((static_cast<uint64_t>(nanos) % 1000000000ULL) << 32) / 1000000000ULL;
    return (seconds << 32) | fraction;
}

bool TimingSync::listen(uint16_t& port)
{
    fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
        SPDLOG_ERROR("[timing] socket() failed: {}", std::strerror(errno));
        return false;
    }
    int off = 0;
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        SPDLOG_ERROR("[timing] bind failed: {}", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    sockaddr_in6 bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) < 0)
    {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    port = ntohs(bound.sin6_port);
    port_ = port;
    return true;
}

void TimingSync::start(const std::string& peer_host, uint16_t peer_port, uint32_t scope_id)
{
    if (fd_ < 0 || peer_port == 0)
    {
        return;
    }
    peer_ = {};
    peer_.sin6_family = AF_INET6;
    peer_.sin6_port = htons(peer_port);
    peer_.sin6_scope_id = scope_id;
    if (::inet_pton(AF_INET6, peer_host.c_str(), &peer_.sin6_addr) != 1)
    {
        SPDLOG_ERROR("[timing] cannot parse peer address '{}'", peer_host);
        return;
    }

    run_.store(true);
    thread_ = std::thread([this] { loop(); });
    SPDLOG_INFO("[timing] driving clock sync to [{}]:{} from local port {}", peer_host, peer_port,
                port_);
}

void TimingSync::stop()
{
    if (run_.exchange(false) && thread_.joinable())
    {
        thread_.join();
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

void TimingSync::sendRequest()
{
    uint8_t packet[kPacketSize] = {};
    packet[0] = 0x80;  // version 2
    packet[1] = kPayloadRequest;
    packet[2] = 0x00;
    packet[3] = 0x07;  // length in 32-bit words minus one

    // Our transmit time (T1) goes in ntpTransmit; the phone echoes it back as
    // ntpOriginate so we can pair the response with this request.
    const uint64_t t1 = syncedNtp();
    pending_t1_ = t1;
    writeNtpImpl(packet + 24, t1);

    if (::sendto(fd_, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer_),
                 sizeof(peer_)) < 0)
    {
        SPDLOG_DEBUG("[timing] sendto failed: {}", std::strerror(errno));
    }
}

void TimingSync::handlePacket(const uint8_t* data, size_t length, const sockaddr_in6& from)
{
    if (length < kPacketSize)
    {
        return;
    }

    if (data[1] == kPayloadRequest)
    {
        // The phone is syncing to us: echo its transmit time as our originate,
        // then stamp our receive (T2) and transmit (T3).
        uint8_t response[kPacketSize] = {};
        response[0] = 0x80;
        response[1] = kPayloadResponse;
        response[3] = 0x07;
        std::memcpy(response + 8, data + 24, 8);
        writeNtpImpl(response + 16, syncedNtp());
        writeNtpImpl(response + 24, syncedNtp());
        ::sendto(fd_, response, sizeof(response), 0,
                 reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        return;
    }

    if (data[1] != kPayloadResponse)
    {
        return;
    }

    // offset = ((T2 - T1) + (T3 - T4)) / 2
    const uint64_t t4 = syncedNtp();
    const uint64_t t1 = readNtpImpl(data + 8);
    const uint64_t t2 = readNtpImpl(data + 16);
    const uint64_t t3 = readNtpImpl(data + 24);

    if (pending_t1_ == 0 || t1 != pending_t1_)
    {
        return;  // stale or duplicate
    }
    pending_t1_ = 0;

    const double offset = ntp::offsetSeconds(t1, t2, t3, t4);

    constexpr double kStepThresholdSeconds = 0.128;
    constexpr double kSlewGain = 1.0 / 8.0;

    if (!synced_)
    {
        // Adopt the phone's transmit time outright rather than stepping by
        // `offset`. The two clocks are ~126 years apart at this point -- ours
        // counts from boot, the phone's from the NTP epoch -- which is past the
        // +/-68 year window a signed NTP difference can represent, so `offset`
        // has wrapped and carries the wrong sign entirely. Measured on
        // hardware: a true gap of +3.99e9 s computed as -3.05e8 s, after which
        // the 1/8 slew took over ninety seconds to crawl back.
        //
        // Adopting costs the half round trip we do not subtract here; the slew
        // below removes it within a few samples.
        clock_offset_ns_ = ntp::toNanos(t3) - rawNowNs();
        synced_ = true;
        SPDLOG_INFO("[timing] clock adopted from the phone (offset {:.3f} s)",
                    static_cast<double>(clock_offset_ns_) / 1e9);
        return;
    }

    clock_offset_ns_ += static_cast<int64_t>(offset * kSlewGain * 1e9);

    if (std::abs(offset) > kStepThresholdSeconds)
    {
        SPDLOG_DEBUG("[timing] large phase error {:.3f} s", offset);
    }
}

void TimingSync::loop()
{
    auto next_request = std::chrono::steady_clock::now();

    while (run_.load())
    {
        if (std::chrono::steady_clock::now() >= next_request)
        {
            sendRequest();
            next_request += kRequestInterval;
        }

        pollfd pfd{fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 100) <= 0)
        {
            continue;
        }

        uint8_t buffer[256];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = ::recvfrom(fd_, buffer, sizeof(buffer), 0,
                                     reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n > 0)
        {
            handlePacket(buffer, static_cast<size_t>(n), from);
        }
    }
}

}  // namespace airplay
