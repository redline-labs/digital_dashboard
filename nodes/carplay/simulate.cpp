// SPDX-License-Identifier: GPL-3.0-or-later
#include "simulate.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace carplay
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

// Draws a moving test pattern into a YUV420P frame: a scrolling colour ramp
// plus a box that sweeps across, so dropped/stale frames are obvious by eye.
void drawTestPattern(AVFrame* frame, int64_t index)
{
    const int w = frame->width;
    const int h = frame->height;
    const int phase = static_cast<int>(index);

    for (int y = 0; y < h; ++y)
    {
        uint8_t* row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < w; ++x)
        {
            row[x] = static_cast<uint8_t>((x + phase * 3) ^ (y + phase));
        }
    }
    for (int y = 0; y < h / 2; ++y)
    {
        uint8_t* u = frame->data[1] + y * frame->linesize[1];
        uint8_t* v = frame->data[2] + y * frame->linesize[2];
        for (int x = 0; x < w / 2; ++x)
        {
            u[x] = static_cast<uint8_t>(128 + 64 * std::sin((x + phase) * 0.05));
            v[x] = static_cast<uint8_t>(128 + 64 * std::cos((y + phase) * 0.05));
        }
    }

    // Sweeping white box.
    const int box = std::max(8, w / 12);
    const int bx = static_cast<int>((index * 7) % std::max(1, w - box));
    const int by = (h - box) / 2;
    for (int y = by; y < by + box && y < h; ++y)
    {
        std::memset(frame->data[0] + y * frame->linesize[0] + bx, 235, static_cast<size_t>(box));
    }
}

bool isKeyframePacket(const AVPacket* pkt)
{
    return (pkt->flags & AV_PKT_FLAG_KEY) != 0;
}

// Rotating fake media metadata so the now_playing widget visibly updates.
struct FakeTrack
{
    const char* title;
    const char* artist;
    const char* album;
    float duration;
};

const FakeTrack kTracks[] = {
    {"Autobahn", "Kraftwerk", "Autobahn", 223.0f},
    {"Cissy Strut", "The Meters", "The Meters", 185.0f},
    {"Nightcall", "Kavinsky", "OutRun", 258.0f},
};

const char* kRoads[] = {"Mulholland Dr", "Laurel Canyon Blvd", "Sunset Blvd", "US-101 N"};

}  // namespace

bool runSimulation(ZenohBridge& bridge, std::atomic<bool>& stop, int width, int height, int fps)
{
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (encoder == nullptr)
    {
        SPDLOG_ERROR("[sim] no H.264 encoder available in this libavcodec build");
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    if (ctx == nullptr)
    {
        SPDLOG_ERROR("[sim] avcodec_alloc_context3 failed");
        return false;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, fps};
    ctx->framerate = AVRational{fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = fps * 2;  // a keyframe every 2s, like a real projection stream
    ctx->max_b_frames = 0;
    ctx->bit_rate = 4'000'000;
    // GLOBAL_HEADER makes the encoder hand us SPS/PPS in extradata so we can
    // publish them as a config message, mirroring the real AirPlay
    // VideoConfig. Without it x264 only emits them in band and extradata stays
    // empty, so no config message would ever be published.
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(ctx, encoder, nullptr) < 0)
    {
        SPDLOG_ERROR("[sim] avcodec_open2 failed");
        avcodec_free_context(&ctx);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    if (av_frame_get_buffer(frame, 0) < 0)
    {
        SPDLOG_ERROR("[sim] av_frame_get_buffer failed");
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }
    AVPacket* pkt = av_packet_alloc();

    SPDLOG_INFO("[sim] simulating a CarPlay session: {}x{} @ {} fps H.264 + 440 Hz tone + metadata",
                width, height, fps);

    // Parameter sets, republished before every keyframe. zenoh has no retained
    // messages, so a one-shot config would leave any subscriber that starts
    // later (or restarts) unable to sync.
    const auto publishConfig = [&bridge, ctx, width, height]() {
        if (ctx->extradata == nullptr || ctx->extradata_size <= 0)
        {
            return;
        }
        VideoFrame cfg;
        cfg.is_config = true;
        cfg.width_px = static_cast<uint16_t>(width);
        cfg.height_px = static_cast<uint16_t>(height);
        cfg.data = ctx->extradata;
        cfg.len = static_cast<size_t>(ctx->extradata_size);
        bridge.publishVideo(cfg);
    };

    if (ctx->extradata != nullptr && ctx->extradata_size > 0)
    {
        SPDLOG_INFO("[sim] H.264 parameter sets: {} bytes (republished each keyframe)",
                    ctx->extradata_size);
        publishConfig();
    }
    else
    {
        SPDLOG_WARN("[sim] encoder produced no extradata; relying on in-band parameter sets");
    }

    const auto started = std::chrono::steady_clock::now();
    const auto frame_interval = std::chrono::microseconds(1'000'000 / fps);
    auto next_frame = started;

    // Audio: 20 ms of 440 Hz stereo s16 per tick.
    constexpr uint32_t kAudioRate = 48000;
    constexpr uint8_t kAudioChannels = 2;
    const size_t audio_samples_per_tick = kAudioRate / 50;
    std::vector<int16_t> tone(audio_samples_per_tick * kAudioChannels);
    double tone_phase = 0.0;
    auto next_audio = started;

    auto next_meta = started;
    size_t track_index = 0;
    uint32_t art_seq = 1;

    int64_t index = 0;
    while (!stop.load())
    {
        const auto now = std::chrono::steady_clock::now();

        // ---- video ----
        if (now >= next_frame)
        {
            if (av_frame_make_writable(frame) < 0)
            {
                break;
            }
            drawTestPattern(frame, index);
            frame->pts = index;

            if (avcodec_send_frame(ctx, frame) == 0)
            {
                while (avcodec_receive_packet(ctx, pkt) == 0)
                {
                    // Re-send the parameter sets ahead of each keyframe so a
                    // late subscriber can sync within one GOP.
                    if (isKeyframePacket(pkt))
                    {
                        publishConfig();
                    }

                    VideoFrame vf;
                    vf.codec = VideoCodec::H264;
                    vf.is_config = false;
                    vf.is_keyframe = isKeyframePacket(pkt);
                    vf.width_px = static_cast<uint16_t>(width);
                    vf.height_px = static_cast<uint16_t>(height);
                    vf.pts_usec = static_cast<uint64_t>(index) * 1'000'000ull / static_cast<uint64_t>(fps);
                    vf.data = pkt->data;
                    vf.len = static_cast<size_t>(pkt->size);
                    bridge.publishVideo(vf);
                    av_packet_unref(pkt);
                }
            }
            ++index;
            next_frame += frame_interval;

            if (index % (fps * 5) == 0)
            {
                SPDLOG_INFO("[sim] published {} frames", index);
            }
        }

        // ---- audio ----
        if (now >= next_audio)
        {
            for (size_t i = 0; i < audio_samples_per_tick; ++i)
            {
                const auto sample = static_cast<int16_t>(6000.0 * std::sin(tone_phase));
                tone[i * 2 + 0] = sample;
                tone[i * 2 + 1] = sample;
                tone_phase += 2.0 * kPi * 440.0 / kAudioRate;
                if (tone_phase > 2.0 * kPi)
                {
                    tone_phase -= 2.0 * kPi;
                }
            }

            AudioChunk chunk;
            chunk.sample_rate_hz = kAudioRate;
            chunk.channels = kAudioChannels;
            chunk.stream = AudioStream::Music;
            chunk.pcm = reinterpret_cast<const uint8_t*>(tone.data());
            chunk.len = tone.size() * sizeof(int16_t);
            bridge.publishAudio(chunk);

            next_audio += std::chrono::milliseconds(20);
        }

        // ---- session + metadata (1 Hz) ----
        if (now >= next_meta)
        {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - started).count();

            SessionState state;
            state.device_connected = true;
            state.phase = SessionPhase::Recording;
            state.main_width_px = static_cast<uint16_t>(width);
            state.main_height_px = static_cast<uint16_t>(height);
            state.device_name = "Simulated iPhone";
            // Exercise the widget's mic start/stop path for ~5s each minute.
            state.mic_active = (elapsed % 60) >= 55;
            state.mic_sample_rate_hz = 16000;
            state.mic_channels = 1;
            bridge.publishSession(state);

            const FakeTrack& track = kTracks[track_index % std::size(kTracks)];
            const auto track_elapsed = static_cast<float>(elapsed % static_cast<long>(track.duration));
            if (track_elapsed < 1.0f && elapsed > 0)
            {
                track_index++;
                art_seq++;
            }

            NowPlaying np;
            np.title = track.title;
            np.artist = track.artist;
            np.album = track.album;
            np.app = "Simulated Music";
            np.duration_sec = track.duration;
            np.elapsed_sec = track_elapsed;
            np.playing = true;
            np.album_art_seq = art_seq;
            bridge.publishNowPlaying(np);

            NavGuidance nav;
            nav.active = true;
            nav.road_name = kRoads[(elapsed / 10) % std::size(kRoads)];
            nav.after_road_name = kRoads[((elapsed / 10) + 1) % std::size(kRoads)];
            nav.destination_name = "Home";
            nav.maneuver_type = static_cast<uint16_t>((elapsed / 10) % 8);
            nav.maneuver_angle_deg = static_cast<int16_t>(-90 + (elapsed % 180));
            nav.distance_to_maneuver_m = 800.0f - static_cast<float>(elapsed % 800);
            nav.distance_remaining_m = 12000.0f - static_cast<float>(elapsed * 5);
            nav.time_remaining_sec = 900.0f - static_cast<float>(elapsed);
            nav.eta_epoch_sec = static_cast<uint64_t>(std::time(nullptr)) + 900;
            bridge.publishNav(nav);

            next_meta += std::chrono::seconds(1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    SPDLOG_INFO("[sim] stopping after {} frames", index);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return true;
}

}  // namespace carplay
