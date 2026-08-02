// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/mic_uplink.h"

#include "airplay/crypto.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace airplay
{

namespace mic
{

void swapSampleEndianness(Bytes& pcm)
{
    for (size_t i = 0; i + 1 < pcm.size(); i += 2)
    {
        std::swap(pcm[i], pcm[i + 1]);
    }
}

size_t frameBytes(size_t samples_per_frame, uint8_t channels)
{
    return samples_per_frame * channels * 2;
}

Bytes buildPacket(const Bytes& body, uint8_t payload_type, uint16_t sequence, uint32_t timestamp,
                  uint64_t nonce_counter, const Bytes& key)
{
    // RTP header, mirror of the downlink layout. SSRC stays zero for CarPlay
    // input streams.
    uint8_t header[12] = {};
    header[0] = 0x80;
    header[1] = static_cast<uint8_t>(payload_type & 0x7F);
    header[2] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(sequence & 0xFF);
    header[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
    header[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    header[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    header[7] = static_cast<uint8_t>(timestamp & 0xFF);

    // The authenticated data is the timestamp and SSRC, not the whole header:
    // the sequence number is not covered.
    const Bytes aad(header + 4, header + 12);
    const Bytes nonce = crypto::nonce64(nonce_counter);
    const Bytes sealed = crypto::chachaSeal(key, nonce, body, aad);

    Bytes packet(header, header + 12);
    packet.insert(packet.end(), sealed.begin(), sealed.end());
    const Bytes nonce8(nonce.end() - 8, nonce.end());
    packet.insert(packet.end(), nonce8.begin(), nonce8.end());
    return packet;
}

}  // namespace mic

MicUplink::~MicUplink()
{
    stop();
}

void MicUplink::setStatusHandler(StatusHandler handler)
{
    status_handler_ = std::move(handler);
}

void MicUplink::start(const Peer& peer, uint16_t phone_port, const Bytes& verify_shared,
                      uint32_t sample_rate, uint8_t channels, int stream_type,
                      uint64_t connection_id, size_t frames_per_packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0)
    {
        return;  // already up
    }

    fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
        SPDLOG_ERROR("[audio] mic uplink socket() failed: {}", std::strerror(errno));
        return;
    }

    dest_ = {};
    dest_.sin6_family = AF_INET6;
    dest_.sin6_port = htons(phone_port);
    dest_.sin6_scope_id = peer.scope_id;
    if (::inet_pton(AF_INET6, peer.address.c_str(), &dest_.sin6_addr) != 1)
    {
        SPDLOG_ERROR("[audio] mic uplink: cannot parse peer '{}'", peer.address);
        ::close(fd_);
        fd_ = -1;
        return;
    }

    // The input key mirrors the downlink (output) key: same DataStream salt
    // (this stream's connection id), the Input rather than Output label.
    const std::string cid_text = std::to_string(connection_id);
    key_ = crypto::hkdfSha512(verify_shared, "DataStream-Salt" + cid_text,
                                         "DataStream-Input-Encryption-Key", 32);

    // Frame granularity: the phone's framesPerPacket if it named one, else
    // 20 ms worth.
    samples_per_frame_ =
        frames_per_packet > 0 ? frames_per_packet : (sample_rate * 20 / 1000);

    sample_rate_ = sample_rate;
    channels_ = channels;
    payload_type_ = stream_type;
    accum_.clear();
    seq_ = 0;
    ts_ = 0;
    nonce_ = 0;
    active_ = true;

    SPDLOG_INFO("[audio] mic uplink up: [{}]:{} {} Hz {} ch, {} samples/frame",
                peer.address, phone_port, sample_rate, channels,
                samples_per_frame_);

    if (status_handler_)
    {
        status_handler_(true, sample_rate, channels);
    }
}

void MicUplink::stop()
{
    bool was_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        was_active = active_;
        active_ = false;
        accum_.clear();
    }
    if (was_active && status_handler_)
    {
        status_handler_(false, 0, 0);
    }
}

void MicUplink::feed(const Bytes& pcm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || fd_ < 0 || samples_per_frame_ == 0)
    {
        return;
    }

    accum_.insert(accum_.end(), pcm.begin(), pcm.end());
    const size_t frame_bytes = mic::frameBytes(samples_per_frame_, channels_);
    if (frame_bytes == 0)
    {
        return;
    }

    while (accum_.size() >= frame_bytes)
    {
        Bytes body(accum_.begin(), accum_.begin() + static_cast<long>(frame_bytes));
        mic::swapSampleEndianness(body);
        accum_.erase(accum_.begin(), accum_.begin() + static_cast<long>(frame_bytes));

        const Bytes packet =
            mic::buildPacket(body, static_cast<uint8_t>(payload_type_), seq_, ts_, nonce_, key_);

        ::sendto(fd_, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));

        seq_ = static_cast<uint16_t>(seq_ + 1);
        ts_ += static_cast<uint32_t>(samples_per_frame_);
        ++nonce_;
    }
}

}  // namespace airplay
