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

void writeNtp(uint8_t* buffer, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        buffer[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xFF);
    }
}

uint64_t readNtp(const uint8_t* buffer)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value = (value << 8) | buffer[i];
    }
    return value;
}

}  // namespace

TimingSync::~TimingSync()
{
    stop();
}

uint64_t TimingSync::syncedNtp() const
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() + clock_offset_ns_;
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
    writeNtp(packet + 24, t1);

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
        writeNtp(response + 16, syncedNtp());
        writeNtp(response + 24, syncedNtp());
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
    const uint64_t t1 = readNtp(data + 8);
    const uint64_t t2 = readNtp(data + 16);
    const uint64_t t3 = readNtp(data + 24);

    if (pending_t1_ == 0 || t1 != pending_t1_)
    {
        return;  // stale or duplicate
    }
    pending_t1_ = 0;

    constexpr double kTwo32 = 4294967296.0;
    const double offset =
        0.5 * ((static_cast<double>(static_cast<int64_t>(t2 - t1)) +
                static_cast<double>(static_cast<int64_t>(t3 - t4)))) /
        kTwo32;

    // The phone's clock runs on its own base, so the first sample is arbitrarily
    // large and simply steps our clock; after that we slew gently.
    constexpr double kStepThresholdSeconds = 0.128;
    constexpr double kSlewGain = 1.0 / 8.0;
    const double applied = synced_ ? offset * kSlewGain : offset;
    clock_offset_ns_ += static_cast<int64_t>(applied * 1e9);

    if (!synced_)
    {
        synced_ = true;
        SPDLOG_INFO("[timing] clock stepped onto the phone's domain (offset {:.3f} s)", offset);
    }
    else if (std::abs(offset) > kStepThresholdSeconds)
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
