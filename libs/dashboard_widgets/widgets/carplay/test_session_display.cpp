// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the CarPlay widget shows as a phone comes and goes, through a real
// widget, a real bus and a real H.264 frame: the bring-up text follows the
// phase, the picture goes when the phone does, a frame still in flight at
// the unplug is not shown, the next phone does not open on the last one's
// screen, and a driver that dies takes its frozen frame with it.
#include "carplay/carplay_widget.h"

#include "pub_sub/zenoh_publisher.h"

#include <QApplication>
#include <QImage>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;

namespace
{

int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
}

template <typename Condition>
bool pumpUntil(Condition condition, std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition())
        {
            return true;
        }
        QApplication::processEvents();
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

// One all-white IDR frame with its parameter sets in band, so it is a sync
// point on its own. Empty if there is no libx264 here.
std::vector<uint8_t> encodeWhiteKeyframe(int width, int height)
{
    std::vector<uint8_t> out;
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (encoder == nullptr)
    {
        return out;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{1, 30};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = 1;
    ctx->max_b_frames = 0;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_open2(ctx, encoder, nullptr) == 0)
    {
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 0) == 0)
        {
            // Studio-range white: Y 235, chroma neutral.
            for (int y = 0; y < height; ++y)
            {
                std::memset(frame->data[0] + y * frame->linesize[0], 235, static_cast<size_t>(width));
            }
            for (int y = 0; y < height / 2; ++y)
            {
                std::memset(frame->data[1] + y * frame->linesize[1], 128, static_cast<size_t>(width / 2));
                std::memset(frame->data[2] + y * frame->linesize[2], 128, static_cast<size_t>(width / 2));
            }
            frame->pts = 0;
            avcodec_send_frame(ctx, frame);
            avcodec_send_frame(ctx, nullptr);
            while (avcodec_receive_packet(ctx, pkt) == 0)
            {
                out.insert(out.end(), pkt->data, pkt->data + pkt->size);
                av_packet_unref(pkt);
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::warn);

    const std::vector<uint8_t> keyframe = encodeWhiteKeyframe(320, 240);
    if (keyframe.empty())
    {
        SPDLOG_WARN("SKIP: no libx264 to make a frame with");
        return PROJECT_TEST_SKIP_CODE;
    }

    const std::string prefix = "test/carplay_display_" + std::to_string(::getpid());
    CarplayConfig_t cfg;
    cfg.video_key = prefix + "/video";
    cfg.audio_key = prefix + "/audio";
    cfg.mic_key = prefix + "/mic";
    cfg.input_key = prefix + "/input";
    cfg.session_key = prefix + "/session";
    cfg.visibility_key = prefix + "/visibility";
    cfg.session_stale_after_ms = 500;

    pub_sub::ZenohPublisher<CarPlaySessionState> session_pub(cfg.session_key);
    pub_sub::ZenohPublisher<CarPlayVideo> video_pub(cfg.video_key);

    CarPlayWidget widget(cfg);
    widget.setGeometry(0, 0, 320, 240);
    widget.show();
    pump(300ms);  // let zenoh match the publishers and subscribers

    using Phase = CarPlaySessionState::Phase;
    const auto publishSession = [&](bool connected, Phase phase) {
        session_pub.fields().setDeviceConnected(connected);
        session_pub.fields().setPhase(phase);
        session_pub.put();
    };
    uint32_t seq = 0;
    const auto publishFrame = [&] {
        auto f = video_pub.fields();
        f.setSeq(++seq);
        f.setCodec(CarPlayVideo::Codec::H264);
        f.setIsKeyframe(true);
        f.setWidthPx(320);
        f.setHeightPx(240);
        f.setData(kj::arrayPtr(keyframe.data(), keyframe.size()));
        video_pub.put();
    };
    const auto headline = [&] { return std::string(widget.connectStatus().headline); };
    // A corner, clear of the centred status text.
    const auto cornerIsBright = [&] { return widget.grab().toImage().pixelColor(4, 4).lightness() > 200; };

    // The bring-up text follows the phase.
    const Phase bring_up[] = {Phase::USB_CONFIG, Phase::LOCKDOWN, Phase::NCM_UP, Phase::IAP2,
                              Phase::AIRPLAY_HANDSHAKE};
    for (const Phase phase : bring_up)
    {
        publishSession(true, phase);
        const std::string expected(carplay::connectStatus(true, true, phase).headline);
        check(pumpUntil([&] { return headline() == expected && widget.connectStatus().step > 0; }, 1000ms),
              "phase " + std::to_string(static_cast<int>(phase)) + " shows \"" + expected + "\", got \"" +
                  headline() + "\"");
    }

    // Recording, and a frame: the picture replaces the text.
    publishSession(true, Phase::RECORDING);
    publishFrame();
    check(pumpUntil([&] { return widget.showsVideo(); }, 2000ms), "a live session with a frame shows video");
    check(cornerIsBright(), "and the frame is what is drawn");

    // Unplugged: the frame goes and the prompt comes back.
    publishSession(false, Phase::IDLE);
    check(pumpUntil([&] { return !widget.showsVideo(); }, 1000ms), "an unplug drops the picture");
    check(!cornerIsBright(), "and what is drawn is not the old frame");
    check(headline() == "Connect an iPhone", "and asks for a phone again, got \"" + headline() + "\"");

    // A frame that was in flight at the unplug is not shown.
    publishFrame();
    pump(300ms);
    check(!widget.showsVideo() && !cornerIsBright(), "a frame arriving after the unplug is not drawn");

    // The next phone: its session going live must not revive the old frame.
    publishSession(false, Phase::IDLE);  // the driver's heartbeat
    pump(100ms);
    publishSession(true, Phase::RECORDING);
    check(pumpUntil([&] { return headline() == "Starting CarPlay"; }, 1000ms),
          "a new session before its first frame says it is starting");
    check(!widget.showsVideo() && !cornerIsBright(), "and does not show the previous phone's screen");
    publishFrame();
    check(pumpUntil([&] { return widget.showsVideo(); }, 2000ms), "its own first frame shows");

    // The driver dies mid-session: no frozen frame.
    check(pumpUntil([&] { return !widget.showsVideo(); }, 2000ms), "a silent driver drops the picture");
    check(headline() == "CarPlay unavailable", "and says CarPlay is unavailable, got \"" + headline() + "\"");
    check(!cornerIsBright(), "with nothing of the old frame left");

    std::fprintf(stderr, "%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
