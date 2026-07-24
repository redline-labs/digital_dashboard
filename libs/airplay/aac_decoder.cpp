// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/aac_decoder.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <array>

namespace airplay
{
namespace
{

// MPEG-4 sampling-frequency index, for the AudioSpecificConfig.
int frequencyIndex(uint32_t sample_rate)
{
    switch (sample_rate)
    {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000: return 11;
        default: return -1;
    }
}

// Two-byte AudioSpecificConfig for AAC-LC (object type 2):
//   5 bits object type | 4 bits freq index | 4 bits channel config | padding.
std::array<uint8_t, 2> audioSpecificConfig(int freq_index, uint8_t channels)
{
    constexpr int kAacLcObjectType = 2;
    const uint16_t bits = static_cast<uint16_t>((kAacLcObjectType << 11) |
                                                ((freq_index & 0x0F) << 7) |
                                                ((channels & 0x0F) << 3));
    return {static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits & 0xFF)};
}

}  // namespace

struct AacDecoder::Impl
{
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    ~Impl()
    {
        if (frame != nullptr)
        {
            av_frame_free(&frame);
        }
        if (packet != nullptr)
        {
            av_packet_free(&packet);
        }
        if (ctx != nullptr)
        {
            avcodec_free_context(&ctx);
        }
    }
};

AacDecoder::AacDecoder() : impl_(std::make_unique<Impl>()) {}
AacDecoder::~AacDecoder() = default;

bool AacDecoder::open(uint32_t sample_rate, uint8_t channels)
{
    const int freq_index = frequencyIndex(sample_rate);
    if (freq_index < 0)
    {
        SPDLOG_ERROR("[aac] unsupported sample rate {}", sample_rate);
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (decoder == nullptr)
    {
        SPDLOG_ERROR("[aac] libavcodec has no AAC decoder");
        return false;
    }

    impl_->ctx = avcodec_alloc_context3(decoder);
    if (impl_->ctx == nullptr)
    {
        return false;
    }

    impl_->ctx->sample_rate = static_cast<int>(sample_rate);
    av_channel_layout_default(&impl_->ctx->ch_layout, channels);

    // Raw AAC (not ADTS): hand the decoder the AudioSpecificConfig as extradata
    // so it knows the profile/rate/channels without an in-band header.
    const auto asc = audioSpecificConfig(freq_index, channels);
    impl_->ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (impl_->ctx->extradata == nullptr)
    {
        return false;
    }
    std::copy(asc.begin(), asc.end(), impl_->ctx->extradata);
    impl_->ctx->extradata_size = static_cast<int>(asc.size());

    if (avcodec_open2(impl_->ctx, decoder, nullptr) < 0)
    {
        SPDLOG_ERROR("[aac] failed to open the AAC decoder ({} Hz, {} ch)", sample_rate,
                     channels);
        return false;
    }

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (impl_->packet == nullptr || impl_->frame == nullptr)
    {
        return false;
    }

    SPDLOG_INFO("[aac] decoder ready: AAC-LC {} Hz {} ch", sample_rate, channels);
    return true;
}

bool AacDecoder::decode(const std::vector<uint8_t>& access_unit, std::vector<uint8_t>& pcm_out)
{
    if (impl_->ctx == nullptr || access_unit.empty())
    {
        return false;
    }

    impl_->packet->data = const_cast<uint8_t*>(access_unit.data());
    impl_->packet->size = static_cast<int>(access_unit.size());

    if (avcodec_send_packet(impl_->ctx, impl_->packet) < 0)
    {
        return false;
    }

    bool produced = false;
    while (avcodec_receive_frame(impl_->ctx, impl_->frame) == 0)
    {
        const AVFrame* f = impl_->frame;
        const int channels = f->ch_layout.nb_channels;
        const int samples = f->nb_samples;
        const auto format = static_cast<AVSampleFormat>(f->format);

        // AAC decodes to planar float (AV_SAMPLE_FMT_FLTP); convert to
        // interleaved S16 for the sink. Handle the common S16 cases too.
        for (int s = 0; s < samples; ++s)
        {
            for (int c = 0; c < channels; ++c)
            {
                int16_t value = 0;
                if (format == AV_SAMPLE_FMT_FLTP)
                {
                    const float sample =
                        reinterpret_cast<const float*>(f->data[c])[s];
                    const float clamped = sample < -1.0f ? -1.0f : (sample > 1.0f ? 1.0f : sample);
                    value = static_cast<int16_t>(clamped * 32767.0f);
                }
                else if (format == AV_SAMPLE_FMT_S16P)
                {
                    value = reinterpret_cast<const int16_t*>(f->data[c])[s];
                }
                else if (format == AV_SAMPLE_FMT_S16)
                {
                    value = reinterpret_cast<const int16_t*>(f->data[0])[s * channels + c];
                }
                pcm_out.push_back(static_cast<uint8_t>(value & 0xFF));
                pcm_out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            }
        }
        produced = true;
    }
    return produced;
}

}  // namespace airplay
