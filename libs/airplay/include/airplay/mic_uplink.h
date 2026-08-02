// SPDX-License-Identifier: GPL-3.0-or-later
//
// The microphone uplink: captured audio going the other way, to the phone.
//
// The phone asks for it by putting a `dataPort` of its own in a main-audio
// SETUP -- that is the only signal, and it means Siri or a call has started
// listening. Audio is RTP-framed and ChaCha20-Poly1305 sealed exactly like the
// downlink, but keyed with the Input label rather than Output.
//
// PCM arrives from wherever the vehicle captures it, in whatever sizes that
// produces, and leaves in fixed frames -- so this accumulates a remainder
// between calls rather than padding, which would inject clicks.
#ifndef AIRPLAY_MIC_UPLINK_H_
#define AIRPLAY_MIC_UPLINK_H_

#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

// The wire form of one microphone packet, split out from the socket so it can
// be checked without one. Two details here are inaudible when wrong in the
// sense that nothing errors -- the audio simply arrives as clicks or noise --
// which is why they are worth pinning down:
//
//   - PCM arrives from the capture side as S16LE and must leave big-endian.
//   - The body is exactly one frame. Padding a short one, rather than holding
//     the remainder back for the next packet, injects a discontinuity every
//     time the capture size does not divide evenly.
namespace mic
{

// In-place S16LE -> S16BE. A trailing odd byte is left alone; it belongs to a
// sample whose second half has not arrived.
void swapSampleEndianness(Bytes& pcm);

// One packet: a 12-byte RTP header, the sealed body, then the 8-byte
// little-endian nonce. `body` must already be big-endian and exactly one
// frame's worth.
Bytes buildPacket(const Bytes& body, uint8_t payload_type, uint16_t sequence, uint32_t timestamp,
                  uint64_t nonce_counter, const Bytes& key);

// Bytes in one frame at this format, or 0 if the format is unusable.
size_t frameBytes(size_t samples_per_frame, uint8_t channels);

}  // namespace mic

class MicUplink
{
  public:
    // Where the phone is. The address is an IPv6 link-local on the NCM link, so
    // the scope id is part of the address, not optional.
    struct Peer
    {
        std::string address;
        uint32_t scope_id = 0;
    };

    // Called when an uplink comes up or goes down, with the format the phone
    // asked for, so the vehicle can start and stop capturing.
    using StatusHandler = std::function<void(bool active, uint32_t sample_rate, uint8_t channels)>;

    MicUplink() = default;
    ~MicUplink();

    MicUplink(const MicUplink&) = delete;
    MicUplink& operator=(const MicUplink&) = delete;

    void setStatusHandler(StatusHandler handler);

    // Brings the uplink up. A second call while one is live is ignored.
    // `connection_id` salts the key derivation and must be the stream's, and
    // `frames_per_packet` is what the phone asked for (0 selects 20 ms).
    void start(const Peer& peer, uint16_t phone_port, const Bytes& verify_shared,
               uint32_t sample_rate, uint8_t channels, int stream_type, uint64_t connection_id,
               size_t frames_per_packet);

    void stop();

    // Feeds captured PCM (S16LE interleaved, at the rate and channel count the
    // phone asked for). A no-op when no uplink is active. Safe from any thread.
    void feed(const Bytes& pcm);

  private:
    // Guarded because feed() runs on the capture thread while start/stop run on
    // the RTSP session thread.
    std::mutex mutex_;
    StatusHandler status_handler_;

    bool active_ = false;
    int fd_ = -1;
    sockaddr_in6 dest_{};
    Bytes key_;
    uint32_t sample_rate_ = 0;
    uint8_t channels_ = 0;
    int payload_type_ = 100;
    size_t samples_per_frame_ = 0;  // PCM framing granularity
    Bytes accum_;                   // leftover PCM between feed() calls
    uint16_t seq_ = 0;
    uint32_t ts_ = 0;
    uint64_t nonce_ = 0;
};

}  // namespace airplay

#endif  // AIRPLAY_MIC_UPLINK_H_
