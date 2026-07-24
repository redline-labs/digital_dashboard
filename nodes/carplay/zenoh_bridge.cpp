// SPDX-License-Identifier: GPL-3.0-or-later
#include "zenoh_bridge.h"

#include <spdlog/spdlog.h>

namespace carplay
{

namespace
{

CarPlayVideo::Codec toCapnp(VideoCodec c)
{
    return (c == VideoCodec::H265) ? CarPlayVideo::Codec::H265 : CarPlayVideo::Codec::H264;
}

CarPlayAudio::StreamType toCapnp(AudioStream s)
{
    switch (s)
    {
        case AudioStream::NavPrompt: return CarPlayAudio::StreamType::NAV_PROMPT;
        case AudioStream::Siri:      return CarPlayAudio::StreamType::SIRI;
        case AudioStream::Call:      return CarPlayAudio::StreamType::CALL;
        case AudioStream::Mic:       return CarPlayAudio::StreamType::MIC;
        case AudioStream::Music:
        default:                     return CarPlayAudio::StreamType::MUSIC;
    }
}

CarPlaySessionState::Phase toCapnp(SessionPhase p)
{
    switch (p)
    {
        case SessionPhase::UsbConfig:        return CarPlaySessionState::Phase::USB_CONFIG;
        case SessionPhase::Lockdown:         return CarPlaySessionState::Phase::LOCKDOWN;
        case SessionPhase::Iap2:             return CarPlaySessionState::Phase::IAP2;
        case SessionPhase::NcmUp:            return CarPlaySessionState::Phase::NCM_UP;
        case SessionPhase::AirplayHandshake: return CarPlaySessionState::Phase::AIRPLAY_HANDSHAKE;
        case SessionPhase::Recording:        return CarPlaySessionState::Phase::RECORDING;
        case SessionPhase::Error:            return CarPlaySessionState::Phase::ERROR;
        case SessionPhase::Idle:
        default:                             return CarPlaySessionState::Phase::IDLE;
    }
}

CarPlayCall::State toCapnp(CallPhase p)
{
    switch (p)
    {
        case CallPhase::Incoming:     return CarPlayCall::State::INCOMING;
        case CallPhase::Dialing:      return CarPlayCall::State::DIALING;
        case CallPhase::Active:       return CarPlayCall::State::ACTIVE;
        case CallPhase::Held:         return CarPlayCall::State::HELD;
        case CallPhase::Disconnected: return CarPlayCall::State::DISCONNECTED;
        case CallPhase::Idle:
        default:                      return CarPlayCall::State::IDLE;
    }
}

InputEvent::Kind fromCapnp(CarPlayInput::Kind k)
{
    switch (k)
    {
        case CarPlayInput::Kind::TOUCH_MOVE: return InputEvent::Kind::TouchMove;
        case CarPlayInput::Kind::TOUCH_UP:   return InputEvent::Kind::TouchUp;
        case CarPlayInput::Kind::KNOB:       return InputEvent::Kind::Knob;
        case CarPlayInput::Kind::MEDIA_KEY:  return InputEvent::Kind::MediaKey;
        case CarPlayInput::Kind::SIRI:       return InputEvent::Kind::Siri;
        case CarPlayInput::Kind::TELEPHONY:  return InputEvent::Kind::Telephony;
        case CarPlayInput::Kind::TOUCH_DOWN:
        default:                             return InputEvent::Kind::TouchDown;
    }
}

AudioStream fromCapnp(CarPlayAudio::StreamType s)
{
    switch (s)
    {
        case CarPlayAudio::StreamType::NAV_PROMPT: return AudioStream::NavPrompt;
        case CarPlayAudio::StreamType::SIRI:       return AudioStream::Siri;
        case CarPlayAudio::StreamType::CALL:       return AudioStream::Call;
        case CarPlayAudio::StreamType::MIC:        return AudioStream::Mic;
        case CarPlayAudio::StreamType::MUSIC:
        default:                                   return AudioStream::Music;
    }
}

}  // namespace

ZenohBridge::ZenohBridge(const std::string& key_prefix) :
    prefix_(key_prefix),
    video_pub_(key_prefix + "/video"),
    audio_pub_(key_prefix + "/audio"),
    session_pub_(key_prefix + "/session"),
    nav_pub_(key_prefix + "/nav"),
    nowplaying_pub_(key_prefix + "/nowplaying"),
    call_pub_(key_prefix + "/call")
{
    SPDLOG_INFO("[node] zenoh bridge publishing under '{}/'", prefix_);
}

void ZenohBridge::publishVideo(const VideoFrame& frame)
{
    if (frame.data == nullptr || frame.len == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(video_mutex_);
    auto& f = video_pub_.fields();
    f.setSeq(++video_seq_);
    f.setCodec(toCapnp(frame.codec));
    f.setIsConfig(frame.is_config);
    f.setIsKeyframe(frame.is_keyframe);
    f.setWidthPx(frame.width_px);
    f.setHeightPx(frame.height_px);
    f.setPtsUsec(frame.pts_usec);
    f.setData(kj::arrayPtr(frame.data, frame.len));
    video_pub_.put();

    // Very chatty; only meaningful during bring-up.
    SPDLOG_TRACE("[node] video seq={} {} bytes cfg={} key={}",
                 video_seq_, frame.len, frame.is_config, frame.is_keyframe);
}

void ZenohBridge::publishAudio(const AudioChunk& chunk)
{
    if (chunk.pcm == nullptr || chunk.len == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(audio_mutex_);
    auto& f = audio_pub_.fields();
    f.setSampleRateHz(chunk.sample_rate_hz);
    f.setChannels(chunk.channels);
    f.setStreamType(toCapnp(chunk.stream));
    f.setPtsUsec(chunk.pts_usec);
    f.setPcm(kj::arrayPtr(chunk.pcm, chunk.len));
    audio_pub_.put();
}

void ZenohBridge::publishSession(const SessionState& state)
{
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto& f = session_pub_.fields();
    f.setDeviceConnected(state.device_connected);
    f.setPhase(toCapnp(state.phase));
    f.setNightMode(state.night_mode);
    f.setMainWidthPx(state.main_width_px);
    f.setMainHeightPx(state.main_height_px);
    f.setDeviceName(state.device_name);
    f.setMicActive(state.mic_active);
    f.setMicSampleRateHz(state.mic_sample_rate_hz);
    f.setMicChannels(state.mic_channels);
    session_pub_.put();

    SPDLOG_DEBUG("[node] session: connected={} phase={} mic={}",
                 state.device_connected, static_cast<int>(state.phase), state.mic_active);
}

void ZenohBridge::publishNav(const NavGuidance& nav)
{
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto& f = nav_pub_.fields();
    f.setActive(nav.active);
    f.setRoadName(nav.road_name);
    f.setAfterRoadName(nav.after_road_name);
    f.setDestinationName(nav.destination_name);
    f.setManeuverType(nav.maneuver_type);
    f.setManeuverAngleDeg(nav.maneuver_angle_deg);
    f.setJunctionType(nav.junction_type);
    f.setDistanceToManeuverM(nav.distance_to_maneuver_m);
    f.setDistanceRemainingM(nav.distance_remaining_m);
    f.setTimeRemainingSec(nav.time_remaining_sec);
    f.setEtaEpochSec(nav.eta_epoch_sec);
    nav_pub_.put();

    SPDLOG_DEBUG("[node] nav: active={} road='{}' maneuver={} in {:.0f}m",
                 nav.active, nav.road_name, nav.maneuver_type, nav.distance_to_maneuver_m);
}

void ZenohBridge::publishNowPlaying(const NowPlaying& np)
{
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto& f = nowplaying_pub_.fields();
    f.setTitle(np.title);
    f.setArtist(np.artist);
    f.setAlbum(np.album);
    f.setApp(np.app);
    f.setDurationSec(np.duration_sec);
    f.setElapsedSec(np.elapsed_sec);
    f.setPlaying(np.playing);
    f.setAlbumArtSeq(np.album_art_seq);
    f.setAlbumArt(kj::arrayPtr(np.album_art.data(), np.album_art.size()));
    nowplaying_pub_.put();

    SPDLOG_DEBUG("[node] nowplaying: '{}' / '{}' playing={} art={}B",
                 np.title, np.artist, np.playing, np.album_art.size());
}

void ZenohBridge::publishCall(const CallState& call)
{
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto& f = call_pub_.fields();
    f.setState(toCapnp(call.phase));
    f.setRemoteName(call.remote_name);
    f.setRemoteNumber(call.remote_number);
    f.setDurationSec(call.duration_sec);
    call_pub_.put();

    SPDLOG_DEBUG("[node] call: state={} remote='{}'", static_cast<int>(call.phase), call.remote_name);
}

void ZenohBridge::setInputHandler(std::function<void(const InputEvent&)> handler)
{
    input_handler_ = std::move(handler);
    input_sub_ = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayInput>>(
        prefix_ + "/input",
        [this](CarPlayInput::Reader reader)
        {
            if (!input_handler_)
            {
                return;
            }
            InputEvent ev;
            ev.kind = fromCapnp(reader.getKind());
            ev.x = reader.getX();
            ev.y = reader.getY();
            ev.code = reader.getCode();
            ev.value = reader.getValue();
            input_handler_(ev);
        });
}

void ZenohBridge::setMicHandler(std::function<void(const AudioChunk&)> handler)
{
    mic_handler_ = std::move(handler);
    mic_sub_ = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayAudio>>(
        prefix_ + "/mic",
        [this](CarPlayAudio::Reader reader)
        {
            if (!mic_handler_)
            {
                return;
            }
            auto pcm = reader.getPcm();
            AudioChunk chunk;
            chunk.sample_rate_hz = reader.getSampleRateHz();
            chunk.channels = reader.getChannels();
            chunk.stream = fromCapnp(reader.getStreamType());
            chunk.pts_usec = reader.getPtsUsec();
            chunk.pcm = pcm.begin();
            chunk.len = pcm.size();
            mic_handler_(chunk);
        });
}

void ZenohBridge::setLocationHandler(std::function<void(const LocationFix&)> handler)
{
    location_handler_ = std::move(handler);
    location_sub_ = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayLocation>>(
        prefix_ + "/location",
        [this](CarPlayLocation::Reader reader)
        {
            if (!location_handler_)
            {
                return;
            }
            LocationFix fix;
            fix.latitude_deg = reader.getLatitudeDeg();
            fix.longitude_deg = reader.getLongitudeDeg();
            fix.altitude_m = reader.getAltitudeM();
            fix.speed_knots = reader.getSpeedKnots();
            fix.course_deg = reader.getCourseDeg();
            fix.satellites = reader.getSatellites();
            fix.hdop = reader.getHdop();
            fix.utc_epoch_ms = reader.getUtcEpochMs();
            fix.valid = reader.getValid();
            location_handler_(fix);
        });
}

}  // namespace carplay
