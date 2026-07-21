// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the CarPlay driver publishes to (and consumes from) zenoh lives
// here, so the protocol libraries stay transport-agnostic and the rest of the
// node never touches Cap'n Proto directly.
#ifndef CARPLAY_ZENOH_BRIDGE_H_
#define CARPLAY_ZENOH_BRIDGE_H_

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "carplay_video.capnp.h"
#include "carplay_audio.capnp.h"
#include "carplay_input.capnp.h"
#include "carplay_session.capnp.h"
#include "carplay_nav.capnp.h"
#include "carplay_nowplaying.capnp.h"
#include "carplay_call.capnp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace carplay
{

// Plain input types so callers never construct Cap'n Proto builders.

enum class VideoCodec
{
    H264,
    H265
};

struct VideoFrame
{
    VideoCodec codec = VideoCodec::H264;
    bool is_config = false;
    bool is_keyframe = false;
    uint16_t width_px = 0;
    uint16_t height_px = 0;
    uint64_t pts_usec = 0;
    const uint8_t* data = nullptr;  // Annex-B access unit
    size_t len = 0;
};

enum class AudioStream
{
    Music,
    NavPrompt,
    Siri,
    Call,
    Mic
};

struct AudioChunk
{
    uint32_t sample_rate_hz = 0;
    uint8_t channels = 0;
    AudioStream stream = AudioStream::Music;
    uint64_t pts_usec = 0;
    const uint8_t* pcm = nullptr;  // s16le interleaved
    size_t len = 0;
};

enum class SessionPhase
{
    Idle,
    UsbConfig,
    Lockdown,
    Iap2,
    NcmUp,
    AirplayHandshake,
    Recording,
    Error
};

struct SessionState
{
    bool device_connected = false;
    SessionPhase phase = SessionPhase::Idle;
    bool night_mode = false;
    uint16_t main_width_px = 0;
    uint16_t main_height_px = 0;
    std::string device_name;
    bool mic_active = false;
    uint32_t mic_sample_rate_hz = 0;
    uint8_t mic_channels = 0;
};

struct NavGuidance
{
    bool active = false;
    std::string road_name;
    std::string after_road_name;
    std::string destination_name;
    uint16_t maneuver_type = 0;
    int16_t maneuver_angle_deg = 0;
    uint16_t junction_type = 0;
    float distance_to_maneuver_m = 0.0f;
    float distance_remaining_m = 0.0f;
    float time_remaining_sec = 0.0f;
    uint64_t eta_epoch_sec = 0;
};

struct NowPlaying
{
    std::string title;
    std::string artist;
    std::string album;
    std::string app;
    float duration_sec = 0.0f;
    float elapsed_sec = 0.0f;
    bool playing = false;
    uint32_t album_art_seq = 0;
    std::vector<uint8_t> album_art;  // JPEG/PNG, only on track change
};

enum class CallPhase
{
    Idle,
    Incoming,
    Dialing,
    Active,
    Held,
    Disconnected
};

struct CallState
{
    CallPhase phase = CallPhase::Idle;
    std::string remote_name;
    std::string remote_number;
    float duration_sec = 0.0f;
};

struct InputEvent
{
    enum class Kind
    {
        TouchDown,
        TouchMove,
        TouchUp,
        Knob,
        MediaKey,
        Siri,
        Telephony
    };

    Kind kind = Kind::TouchDown;
    uint16_t x = 0;  // 0..10000 normalized over the widget
    uint16_t y = 0;
    uint16_t code = 0;
    int32_t value = 0;
};

// Owns every zenoh endpoint for the driver. Publishers are not thread-safe,
// so each is guarded -- the video, audio, and metadata paths run on different
// threads by design.
class ZenohBridge
{
  public:
    explicit ZenohBridge(const std::string& key_prefix);

    // Driver -> dashboard.
    void publishVideo(const VideoFrame& frame);
    void publishAudio(const AudioChunk& chunk);
    void publishSession(const SessionState& state);
    void publishNav(const NavGuidance& nav);
    void publishNowPlaying(const NowPlaying& np);
    void publishCall(const CallState& call);

    // Dashboard -> driver. Callbacks fire on zenoh subscriber threads.
    void setInputHandler(std::function<void(const InputEvent&)> handler);
    void setMicHandler(std::function<void(const AudioChunk&)> handler);

  private:
    std::string prefix_;

    std::mutex video_mutex_;
    pub_sub::ZenohPublisher<CarPlayVideo> video_pub_;

    std::mutex audio_mutex_;
    pub_sub::ZenohPublisher<CarPlayAudio> audio_pub_;

    // Session + metadata are low-rate and share one lock.
    std::mutex meta_mutex_;
    pub_sub::ZenohPublisher<CarPlaySessionState> session_pub_;
    pub_sub::ZenohPublisher<CarPlayNav> nav_pub_;
    pub_sub::ZenohPublisher<CarPlayNowPlaying> nowplaying_pub_;
    pub_sub::ZenohPublisher<CarPlayCall> call_pub_;

    std::function<void(const InputEvent&)> input_handler_;
    std::function<void(const AudioChunk&)> mic_handler_;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayInput>> input_sub_;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayAudio>> mic_sub_;

    uint32_t video_seq_ = 0;
};

}  // namespace carplay

#endif  // CARPLAY_ZENOH_BRIDGE_H_
