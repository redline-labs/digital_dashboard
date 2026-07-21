// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
#include "apple_usb/muxd.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>

namespace apple_usb
{

namespace
{

// usbmux framing.
constexpr uint32_t kMuxMagic = 0xFEEDFACE;
constexpr uint32_t kProtoVersion = 0;
constexpr uint32_t kProtoSetup = 2;
constexpr uint32_t kProtoTcp = 6;

// TCP flags.
constexpr uint8_t kThFin = 0x01;
constexpr uint8_t kThSyn = 0x02;
constexpr uint8_t kThRst = 0x04;
constexpr uint8_t kThAck = 0x10;

// Interface 1 is the multiplexer; endpoints per muxd.py.
constexpr uint8_t kEpOut = 0x04;
constexpr uint8_t kEpIn = 0x85;
constexpr unsigned kMuxInterface = 1;

constexpr uint32_t kTxWindow = 131072;
constexpr size_t kMaxPayload = 16384;

void put_be16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void put_be32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

uint16_t get_be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get_be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

// ---------------- MuxTcpConn ----------------

MuxTcpConn::MuxTcpConn(MuxHost& host, uint16_t sport, uint16_t dport) :
    host_(host), sport_(sport), dport_(dport)
{
}

void MuxTcpConn::sendTcp(uint8_t flags, const uint8_t* payload, size_t payload_len)
{
    // 20-byte TCP header: offset 0x50 (5 words), window scaled down by 8.
    std::vector<uint8_t> th;
    th.reserve(20 + payload_len);
    put_be16(th, sport_);
    put_be16(th, dport_);
    put_be32(th, tx_seq_);
    put_be32(th, tx_ack_);
    th.push_back(0x50);
    th.push_back(flags);
    put_be16(th, static_cast<uint16_t>(kTxWindow >> 8));
    put_be16(th, 0);  // checksum (unused)
    put_be16(th, 0);  // urgent pointer
    if (payload != nullptr && payload_len > 0)
    {
        th.insert(th.end(), payload, payload + payload_len);
    }
    host_.muxSend(kProtoTcp, th.data(), th.size());
}

void MuxTcpConn::onPacket(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t win,
                          const uint8_t* payload, size_t payload_len)
{
    (void)ack;
    if ((flags & kThSyn) && (flags & kThAck))
    {
        tx_seq_ += 1;
        tx_ack_ = seq + 1;
        sendTcp(kThAck);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = true;
        }
        cv_.notify_all();
        return;
    }
    if (flags & kThRst)
    {
        SPDLOG_WARN("[muxd] sport={} dport={} RST from device (win={})", sport_, dport_, win);
        closed_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = true;
            rq_.emplace_back();  // wake recv() with EOF
        }
        cv_.notify_all();
        return;
    }
    if (payload_len > 0)
    {
        tx_ack_ += static_cast<uint32_t>(payload_len);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rq_.emplace_back(payload, payload + payload_len);
        }
        cv_.notify_all();
        sendTcp(kThAck);
    }
    if (flags & kThFin)
    {
        SPDLOG_DEBUG("[muxd] sport={} dport={} FIN from device", sport_, dport_);
        tx_ack_ += 1;
        sendTcp(kThAck);
        closed_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rq_.emplace_back();
        }
        cv_.notify_all();
    }
}

void MuxTcpConn::send(const uint8_t* data, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        const size_t chunk = std::min(kMaxPayload, len - offset);
        sendTcp(kThAck, data + offset, chunk);
        tx_seq_ += static_cast<uint32_t>(chunk);
        offset += chunk;
    }
}

std::vector<uint8_t> MuxTcpConn::recv()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !rq_.empty(); });
    std::vector<uint8_t> out = std::move(rq_.front());
    rq_.pop_front();
    return out;
}

bool MuxTcpConn::waitConnected()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::seconds(5), [this] { return connected_; }))
    {
        return false;
    }
    return !closed_.load();
}

void MuxTcpConn::close()
{
    if (!closed_.exchange(true))
    {
        try
        {
            sendTcp(kThFin | kThAck);
        }
        catch (const std::exception&)
        {
            // Best-effort; the host fd may already be gone.
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rq_.emplace_back();
    }
    cv_.notify_all();
}

// ---------------- MuxHost ----------------

MuxHost::MuxHost(DeviceInfo device) : device_(std::move(device)) {}

MuxHost::~MuxHost()
{
    close();
}

bool MuxHost::open()
{
    const auto fd = openDevice(device_);
    if (!fd)
    {
        return false;
    }
    fd_ = *fd;

    // The kernel may still be settling after the config switch; retry the claim.
    bool claimed = false;
    for (int i = 0; i < 12; ++i)
    {
        try
        {
            usbClaimInterface(fd_, kMuxInterface);
            claimed = true;
            break;
        }
        catch (const std::system_error&)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    if (!claimed)
    {
        SPDLOG_ERROR("[muxd] could not claim mux interface on {}", device_.serial);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    try
    {
        // Version packet: header {proto, length} then {2, 0, 0}.
        std::vector<uint8_t> version;
        put_be32(version, kProtoVersion);
        put_be32(version, 8 + 12);
        put_be32(version, 2);
        put_be32(version, 0);
        put_be32(version, 0);
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            usbBulkOut(fd_, kEpOut, version.data(), version.size());
        }
        // Drain the version reply.
        usbBulkIn(fd_, kEpIn, 65536, 2000);

        const uint8_t setup = 0x07;
        muxSend(kProtoSetup, &setup, 1);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_ERROR("[muxd] handshake failed: {}", e.what());
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    run_.store(true);
    reader_ = std::thread([this] { readerLoop(); });
    return true;
}

void MuxHost::close()
{
    run_.store(false);
    if (reader_.joinable())
    {
        reader_.join();
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

void MuxHost::muxSend(uint32_t proto, const uint8_t* payload, size_t payload_len)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (fd_ < 0)
    {
        return;
    }
    std::vector<uint8_t> pkt;
    pkt.reserve(16 + payload_len);
    put_be32(pkt, proto);
    put_be32(pkt, static_cast<uint32_t>(16 + payload_len));
    put_be32(pkt, kMuxMagic);
    put_be16(pkt, mux_tx_);
    put_be16(pkt, mux_rx_);
    ++mux_tx_;
    if (payload != nullptr && payload_len > 0)
    {
        pkt.insert(pkt.end(), payload, payload + payload_len);
    }
    usbBulkOut(fd_, kEpOut, pkt.data(), pkt.size());
}

void MuxHost::readerLoop()
{
    while (run_.load())
    {
        std::vector<uint8_t> data;
        try
        {
            data = usbBulkIn(fd_, kEpIn, 65536, 1000);
        }
        catch (const std::system_error& e)
        {
            if (e.code().value() == ETIMEDOUT)
            {
                continue;
            }
            if (run_.load())
            {
                SPDLOG_WARN("[muxd] usb reader ended: {}", e.what());
            }
            break;
        }
        if (data.empty())
        {
            continue;
        }

        rxbuf_.insert(rxbuf_.end(), data.begin(), data.end());
        while (rxbuf_.size() >= 8)
        {
            const uint32_t proto = get_be32(rxbuf_.data());
            const uint32_t length = get_be32(rxbuf_.data() + 4);
            if (length < 8 || rxbuf_.size() < length)
            {
                break;
            }

            if (length >= 16)
            {
                mux_rx_ = get_be16(rxbuf_.data() + 12);
            }
            if (proto == kProtoTcp && length >= 36)
            {
                const uint8_t* p = rxbuf_.data() + 16;
                const uint16_t dp = get_be16(p + 2);
                const uint32_t seq = get_be32(p + 4);
                const uint32_t ack = get_be32(p + 8);
                const uint8_t flags = p[13];
                const uint16_t win = get_be16(p + 14);

                std::shared_ptr<MuxTcpConn> conn;
                {
                    std::lock_guard<std::mutex> lock(conns_mutex_);
                    if (auto it = conns_.find(dp); it != conns_.end())
                    {
                        conn = it->second;
                    }
                }
                if (conn)
                {
                    conn->onPacket(flags, seq, ack, win, rxbuf_.data() + 36, length - 36);
                }
            }

            rxbuf_.erase(rxbuf_.begin(), rxbuf_.begin() + length);
        }
    }
}

std::shared_ptr<MuxTcpConn> MuxHost::connect(uint16_t dport)
{
    uint16_t sport;
    std::shared_ptr<MuxTcpConn> conn;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        sport = next_sport_++;
        conn = std::make_shared<MuxTcpConn>(*this, sport, dport);
        conns_[sport] = conn;
    }

    conn->sendTcp(kThSyn);
    if (!conn->waitConnected())
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.erase(sport);
        return nullptr;
    }
    return conn;
}

}  // namespace apple_usb
