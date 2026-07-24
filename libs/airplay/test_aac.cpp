// SPDX-License-Identifier: GPL-3.0-or-later
//
// Round-trips a sine wave through libavcodec's AAC-LC encoder and our
// AacDecoder, verifying that a raw AAC access unit (no ADTS) decodes to sane
// interleaved S16 PCM at the right rate/channels. Exercises the same
// AudioSpecificConfig / extradata path the CarPlay entertainment stream uses,
// without any hardware.
#include "airplay/aac_decoder.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

int g_failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++g_failures;
    }
}

// Encodes `frames` of a 440 Hz sine into raw AAC-LC access units (one per
// vector entry). Returns the encoder's own extradata is unused -- the decoder
// builds its own ASC from rate/channels, which is exactly what we want to test.
std::vector<std::vector<uint8_t>> encodeSine(int sample_rate, int channels, int frames)
{
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (encoder == nullptr)
    {
        SPDLOG_WARN("no AAC encoder in this libavcodec build; skipping AAC round-trip");
        return {};
    }

    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    ctx->sample_rate = sample_rate;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 128000;
    av_channel_layout_default(&ctx->ch_layout, channels);
    // Raw AAC, no ADTS -- matches how the phone ships access units.
    if (avcodec_open2(ctx, encoder, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        return {};
    }

    const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 1024;
    AVFrame* frame = av_frame_alloc();
    frame->nb_samples = frame_size;
    frame->format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&frame->ch_layout, channels);
    av_frame_get_buffer(frame, 0);

    AVPacket* pkt = av_packet_alloc();
    std::vector<std::vector<uint8_t>> out;

    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / sample_rate;
    for (int f = 0; f < frames; ++f)
    {
        av_frame_make_writable(frame);
        for (int s = 0; s < frame_size; ++s)
        {
            const float v = static_cast<float>(0.25 * std::sin(phase));
            phase += step;
            for (int c = 0; c < channels; ++c)
            {
                reinterpret_cast<float*>(frame->data[c])[s] = v;
            }
        }
        frame->pts = f * frame_size;
        if (avcodec_send_frame(ctx, frame) == 0)
        {
            while (avcodec_receive_packet(ctx, pkt) == 0)
            {
                out.emplace_back(pkt->data, pkt->data + pkt->size);
                av_packet_unref(pkt);
            }
        }
    }
    // Flush.
    avcodec_send_frame(ctx, nullptr);
    while (avcodec_receive_packet(ctx, pkt) == 0)
    {
        out.emplace_back(pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    for (const auto [rate, channels] : {std::pair{44100, 2}, std::pair{48000, 2}})
    {
        const auto units = encodeSine(rate, channels, 20);
        if (units.empty())
        {
            SPDLOG_WARN("AAC encoder unavailable; test is a no-op for {} Hz", rate);
            continue;
        }

        airplay::AacDecoder decoder;
        expect(decoder.open(static_cast<uint32_t>(rate), static_cast<uint8_t>(channels)),
               "decoder opens for AAC-LC");

        std::vector<uint8_t> pcm;
        size_t decoded_units = 0;
        for (const auto& unit : units)
        {
            if (decoder.decode(unit, pcm))
            {
                ++decoded_units;
            }
        }

        // The encoder primes with a couple of silent frames; require that the
        // bulk decoded and produced a plausible amount of 16-bit stereo PCM.
        expect(decoded_units >= units.size() / 2, "most access units decoded");
        expect(pcm.size() % (channels * 2) == 0, "PCM is whole S16 frames");
        expect(pcm.size() > static_cast<size_t>(channels) * 2 * 1024 * 5,
               "produced several frames of PCM");

        // The 440 Hz tone should have real amplitude somewhere (not all zero).
        int16_t peak = 0;
        for (size_t i = 0; i + 1 < pcm.size(); i += 2)
        {
            const int16_t sample = static_cast<int16_t>(pcm[i] | (pcm[i + 1] << 8));
            peak = std::max<int16_t>(peak, static_cast<int16_t>(std::abs(sample)));
        }
        expect(peak > 1000, "decoded PCM carries the tone");

        SPDLOG_INFO("AAC {} Hz {} ch: {} units -> {} bytes PCM, peak {}", rate, channels,
                    decoded_units, pcm.size(), peak);
    }

    if (g_failures == 0)
    {
        SPDLOG_INFO("aac tests passed");
        return 0;
    }
    SPDLOG_ERROR("aac tests FAILED ({} failures)", g_failures);
    return 1;
}
