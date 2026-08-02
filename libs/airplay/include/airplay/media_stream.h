// SPDX-License-Identifier: GPL-3.0-or-later
//
// The phone's media streams, and the microphone uplink going back.
//
// Each stream the phone opens in SETUP gets its own socket and its own key, and
// carries one kind of media:
//
//   screen  TCP, ChaCha20-Poly1305 per message, H.264 or H.265 in avcC/hvcC
//           framing, rewritten to Annex-B for the decoder.
//   audio   UDP, RTP-framed and sealed per packet, either big-endian LPCM or
//           AAC-LC access units decoded to PCM.
//   mic     the reverse direction: captured PCM, RTP-framed and sealed, sent to
//           the dataPort the phone named in its main-audio SETUP.
//
// These are loops over a socket with no session state behind them, which is
// what lets them live here rather than inside the RTSP server.
#ifndef AIRPLAY_MEDIA_STREAM_H_
#define AIRPLAY_MEDIA_STREAM_H_

#include "airplay/nalu.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

// Decoded media handed up for publishing.
struct VideoPacket
{
    Bytes data;  // Annex-B
    uint64_t timestamp = 0;
    bool keyframe = false;
    // True for the codec parameter sets rather than a frame. zenoh has no
    // retained messages, so the node republishes these before every keyframe.
    bool is_config = false;
    // The codec the phone announced in the parameter sets, carried on every
    // packet so the node can label the stream without tracking the config
    // itself. The phone chooses this, and picks H.264 in practice; H.265 is
    // wired through because the framing and keyframe rules differ and guessing
    // wrong is silent (see nalu::isKeyframeNalu).
    nalu::Codec codec = nalu::Codec::H264;
};

struct AudioPacket
{
    Bytes data;  // interleaved S16LE PCM
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    // The CarPlay stream type (100 main, 101 alt, 102 entertainment) and the
    // audioType category ("media", "telephony", "speechRecognition", ...) so the
    // node can label the zenoh audio stream.
    int stream_type = 100;
    std::string audio_type;
};

// Accepts the phone's video data connection on `listen_fd` and pumps frames
// until it closes or `run` goes false. `key` is the per-stream key.
//
// `on_keyframe` is called with the steady_clock time of each keyframe or
// parameter set the phone sent, so the caller can tell a stream that is already
// producing them from one that needs nudging.
void runScreenStream(int listen_fd, Bytes key, const std::atomic<bool>& run,
                     const std::function<void(const VideoPacket&)>& on_packet,
                     const std::function<void(int64_t)>& on_keyframe);

// Receives one audio stream on its UDP data port until `run` goes false.
// Consumes `data_fd`.
void runAudioStream(int data_fd, Bytes key, uint32_t sample_rate, uint8_t channels,
                    int stream_type, std::string audio_type, bool is_aac,
                    const std::atomic<bool>& run,
                    const std::function<void(const AudioPacket&)>& on_packet);

}  // namespace airplay

#endif  // AIRPLAY_MEDIA_STREAM_H_
