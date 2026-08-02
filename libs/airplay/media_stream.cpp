// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/media_stream.h"

#include "airplay/aac_decoder.h"
#include "airplay/crypto.h"

#include <spdlog/spdlog.h>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace airplay
{

void runScreenStream(int listen_fd, Bytes key, const std::atomic<bool>& run,
                     const std::function<void(const VideoPacket&)>& on_packet,
                     const std::function<void(int64_t)>& on_keyframe)
{
    constexpr size_t kHeaderLength = 128;
    constexpr size_t kMaxBody = 8 * 1024 * 1024;
    constexpr uint8_t kOpVideoFrame = 0;
    constexpr uint8_t kOpVideoConfig = 1;

    while (run.load())
    {
        pollfd listen_pfd{listen_fd, POLLIN, 0};
        if (::poll(&listen_pfd, 1, 200) <= 0)
        {
            continue;
        }
        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0)
        {
            continue;
        }
        SPDLOG_INFO("[video] screen stream connected");

        Bytes buffer;
        Bytes chunk(65536);
        uint64_t counter = 0;
        uint64_t frames = 0;
        Bytes last_config;  // dedupe the (unchanging) codec-config log
        // Frames carry no codec of their own -- only the parameter sets say
        // what this stream is. Latch it here so every frame after them is
        // parsed by the right rules, and reset per connection because a
        // reconnecting phone may negotiate differently.
        nalu::Codec stream_codec = nalu::Codec::H264;

        // Optional raw Annex-B dump for bring-up: `ffmpeg -i dump.h264 out.png`
        // turns it into a viewable image without a running dashboard.
        std::FILE* dump = nullptr;
        if (const char* path = std::getenv("AIRPLAY_DUMP_VIDEO"); path != nullptr)
        {
            dump = std::fopen(path, "wb");
            SPDLOG_INFO("[video] dumping Annex-B to {}", path);
        }

        while (run.load())
        {
            pollfd pfd{client, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 200);
            if (ready < 0)
            {
                break;
            }
            if (ready == 0)
            {
                continue;
            }
            const ssize_t n = ::recv(client, chunk.data(), chunk.size(), 0);
            if (n <= 0)
            {
                break;
            }
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + n);

            while (buffer.size() >= kHeaderLength)
            {
                const size_t body_size = static_cast<size_t>(buffer[0]) |
                                         (static_cast<size_t>(buffer[1]) << 8) |
                                         (static_cast<size_t>(buffer[2]) << 16) |
                                         (static_cast<size_t>(buffer[3]) << 24);
                if (body_size > kMaxBody)
                {
                    SPDLOG_ERROR("[video] implausible body size {}, dropping connection",
                                 body_size);
                    buffer.clear();
                    break;
                }
                if (buffer.size() < kHeaderLength + body_size)
                {
                    break;
                }

                const Bytes header(buffer.begin(), buffer.begin() + kHeaderLength);
                const Bytes body(buffer.begin() + kHeaderLength,
                                 buffer.begin() + kHeaderLength + body_size);
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<long>(kHeaderLength + body_size));

                const auto mark_sync_point = [&on_keyframe] {
                    if (on_keyframe)
                    {
                        on_keyframe(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                    }
                };

                const uint8_t opcode = header[4];
                if (opcode == kOpVideoConfig)
                {
                    // A config is a sync point; record it so the keyframe thread
                    // does not nudge the phone while the stream already produces
                    // its own. (P-frames must NOT count, or a static screen that
                    // only sends P-frames would never trigger a nudge.)
                    mark_sync_point();
                    const auto config = nalu::configToAnnexB(body);
                    if (!config)
                    {
                        // The phone opens the screen stream with an empty
                        // opcode-1 message; that is not a failure, and warning
                        // about it on every connection trains you to ignore
                        // this line when it does mean something. Log the header
                        // at debug so the message can still be identified.
                        if (body.empty())
                        {
                            SPDLOG_DEBUG("[video] empty codec config at stream start, header "
                                         "{:02x} {:02x} {:02x} {:02x} op={:02x} | {:02x} {:02x} "
                                         "{:02x} {:02x}",
                                         header[0], header[1], header[2], header[3], header[4],
                                         header[5], header[6], header[7], header[8]);
                        }
                        else
                        {
                            SPDLOG_WARN("[video] could not parse the codec config ({} bytes)",
                                        body.size());
                        }
                        continue;
                    }
                    if (config->codec != stream_codec && !last_config.empty())
                    {
                        SPDLOG_INFO("[video] codec switched to {}",
                                    config->codec == nalu::Codec::H265 ? "H.265" : "H.264");
                    }
                    stream_codec = config->codec;

                    // The config repeats before every keyframe (~1/s) and never
                    // changes, so only announce it at INFO when it is new.
                    if (config->annex_b != last_config)
                    {
                        last_config = config->annex_b;
                        SPDLOG_INFO("[video] codec config: {} ({} bytes Annex-B)",
                                    config->codec == nalu::Codec::H265 ? "H.265" : "H.264",
                                    config->annex_b.size());
                    }
                    else
                    {
                        SPDLOG_DEBUG("[video] codec config (unchanged, {} bytes)",
                                     config->annex_b.size());
                    }
                    if (dump != nullptr)
                    {
                        std::fwrite(config->annex_b.data(), 1, config->annex_b.size(), dump);
                    }
                    if (on_packet)
                    {
                        VideoPacket packet;
                        packet.data = config->annex_b;
                        packet.is_config = true;
                        packet.codec = stream_codec;
                        on_packet(packet);
                    }
                }
                else if (opcode == kOpVideoFrame)
                {
                    Bytes payload = body;
                    if (body.size() >= 16)
                    {
                        const auto opened =
                            crypto::chachaOpen(key, crypto::nonce64(counter), body, header);
                        if (!opened)
                        {
                            SPDLOG_WARN("[video] frame {} failed to decrypt", counter);
                            continue;
                        }
                        ++counter;
                        payload = *opened;
                    }

                    const Bytes annex_b = nalu::avccFrameToAnnexB(payload);
                    if (dump != nullptr)
                    {
                        std::fwrite(annex_b.data(), 1, annex_b.size(), dump);
                    }
                    if (++frames == 1)
                    {
                        SPDLOG_INFO("[video] FIRST FRAME decoded: {} bytes Annex-B",
                                    annex_b.size());
                    }

                    if (on_packet)
                    {
                        VideoPacket packet;
                        packet.data = annex_b;
                        packet.codec = stream_codec;
                        packet.keyframe = nalu::annexBContainsKeyframe(annex_b, stream_codec);
                        if (packet.keyframe)
                        {
                            mark_sync_point();  // an in-band keyframe is a sync point too
                        }
                        on_packet(packet);
                    }
                }
            }
        }

        if (dump != nullptr)
        {
            std::fclose(dump);
        }
        SPDLOG_INFO("[video] screen stream closed after {} frames", frames);
        ::close(client);
    }
}

void runAudioStream(int data_fd, Bytes key, uint32_t sample_rate, uint8_t channels,
                    int stream_type, std::string audio_type, bool is_aac,
                    const std::atomic<bool>& run,
                    const std::function<void(const AudioPacket&)>& on_packet)
{
    // Each UDP datagram is [12B RTP header][ciphertext][16B tag][8B nonce LE],
    // sealed with ChaCha20-Poly1305. The AAD is the RTP header's timestamp+SSRC
    // (bytes 4..12), and the nonce is 4 zero bytes followed by the 8-byte tail.
    constexpr size_t kRtpHeader = 12;
    constexpr size_t kTail = 24;  // 16-byte tag + 8-byte nonce

    // For the entertainment stream the decrypted payload is a raw AAC-LC access
    // unit rather than PCM; decode it here.
    AacDecoder aac_decoder;
    if (is_aac && !aac_decoder.open(sample_rate, channels))
    {
        SPDLOG_ERROR("[audio] could not open the AAC decoder for type {}; stream will be silent",
                     stream_type);
        is_aac = false;  // fall through to raw (will sound wrong, but visible)
    }

    Bytes buffer(4096);
    uint64_t packets = 0;

    // Optional raw S16LE dump for isolating choppiness: `aplay -f S16_LE -r
    // <rate> -c <ch> dump.pcm` plays it back straight, bypassing zenoh and the
    // widget. Gaps here mean the problem is upstream (UDP/decrypt/phone).
    std::FILE* dump = nullptr;
    if (const char* path = std::getenv("AIRPLAY_DUMP_AUDIO"); path != nullptr)
    {
        dump = std::fopen(path, "wb");
        SPDLOG_INFO("[audio] dumping S16LE to {} ({} Hz {} ch)", path, sample_rate, channels);
    }
    auto last_recv = std::chrono::steady_clock::now();

    while (run.load())
    {
        pollfd pfd{data_fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 200);
        if (ready < 0)
        {
            break;
        }
        if (ready == 0)
        {
            continue;
        }

        const ssize_t n = ::recv(data_fd, buffer.data(), buffer.size(), 0);
        if (n < 0)
        {
            break;
        }
        if (static_cast<size_t>(n) < kRtpHeader + kTail)
        {
            continue;
        }
        const size_t len = static_cast<size_t>(n);

        const Bytes aad(buffer.begin() + 4, buffer.begin() + kRtpHeader);
        Bytes nonce(4, 0);
        nonce.insert(nonce.end(), buffer.begin() + static_cast<long>(len - 8),
                     buffer.begin() + static_cast<long>(len));
        // chachaOpen wants ciphertext followed by tag; the wire has exactly that
        // between the header and the trailing nonce.
        const Bytes sealed(buffer.begin() + kRtpHeader,
                           buffer.begin() + static_cast<long>(len - 8));

        const auto payload = crypto::chachaOpen(key, nonce, sealed, aad);
        if (!payload)
        {
            SPDLOG_WARN("[audio] type {} packet failed to decrypt", stream_type);
            continue;
        }

        Bytes samples;
        if (is_aac)
        {
            // The decrypted payload is a raw AAC-LC access unit -> decode to
            // interleaved S16LE (already little-endian, so no swap).
            if (!aac_decoder.decode(*payload, samples) || samples.empty())
            {
                continue;
            }
        }
        else
        {
            // Wire PCM is 16-bit big-endian; the sink wants little-endian.
            samples = *payload;
            for (size_t k = 0; k + 1 < samples.size(); k += 2)
            {
                std::swap(samples[k], samples[k + 1]);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const auto gap_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_recv).count();
        last_recv = now;

        if (++packets == 1)
        {
            SPDLOG_INFO("[audio] first packet on type {} '{}' ({} Hz, {} ch)", stream_type,
                        audio_type, sample_rate, channels);
        }
        // A gap much larger than the packet's own duration means the phone (or
        // our recv) stalled -- a genuine source-side dropout, distinct from any
        // playback-side buffering issue.
        else if (gap_ms > 40)
        {
            SPDLOG_WARN("[audio] type {} inter-packet gap {} ms ({} bytes, ~{} ms of audio)",
                        stream_type, gap_ms, samples.size(),
                        sample_rate ? (samples.size() * 1000 / (sample_rate * channels * 2)) : 0);
        }

        if (dump != nullptr)
        {
            std::fwrite(samples.data(), 1, samples.size(), dump);
        }

        if (on_packet)
        {
            AudioPacket packet;
            packet.data = std::move(samples);
            packet.sample_rate = sample_rate;
            packet.channels = channels;
            packet.stream_type = stream_type;
            packet.audio_type = audio_type;
            on_packet(packet);
        }
    }

    if (dump != nullptr)
    {
        std::fclose(dump);
    }
    SPDLOG_INFO("[audio] stream type {} closed after {} packets", stream_type, packets);
    ::close(data_fd);
}

}  // namespace airplay
