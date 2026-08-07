#include "video/video_decoder.h"

#include "helpers/ffmpeg_log.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <cstring>

namespace scope
{

namespace
{

// QImage::Format_RGB32 packs 0xffRRGGBB into a uint32, so the byte order in
// memory follows the host endianness. Matching it exactly is the whole point:
// it makes drawImage a blit rather than a per-pixel conversion.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
constexpr AVPixelFormat kRgb32PixelFormat = AV_PIX_FMT_BGRA;
#else
constexpr AVPixelFormat kRgb32PixelFormat = AV_PIX_FMT_ARGB;
#endif

}  // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder()
{
    destroyDecoder();
}

bool VideoDecoder::ensureDecoder(Codec codec)
{
    if (context_ != nullptr && codec == codec_)
    {
        return true;
    }
    destroyDecoder();

    // libavcodec and libswscale otherwise write straight to stderr, untimed and
    // unfiltered -- including swscale's "no accelerated colorspace conversion
    // found", which is true on arm64 for every destination format and is detail
    // rather than news.
    helpers::routeFfmpegLogsToSpdlog();

    const AVCodecID av_id = (codec == Codec::H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* decoder = avcodec_find_decoder(av_id);
    if (decoder == nullptr)
    {
        SPDLOG_ERROR("[scope/video] no libavcodec decoder for {}",
                     (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264");
        return false;
    }

    context_ = avcodec_alloc_context3(decoder);

    // thread_count is deliberately left at 1. Setting it to 0 selects frame
    // threading, which withholds thread_count - 1 pictures before emitting the
    // first -- measured at four frames in the CarPlay path. On a live stream
    // that is latency; here it is worse than latency, because the last frame of
    // a seek's catch-up is exactly the one being asked for and it would simply
    // never come out.
    if (context_ == nullptr || avcodec_open2(context_, decoder, nullptr) < 0)
    {
        SPDLOG_ERROR("[scope/video] failed to open the video decoder");
        destroyDecoder();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    codec_ = codec;
    synced_ = false;
    SPDLOG_INFO("[scope/video] decoder ready ({})",
                (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264");
    return true;
}

void VideoDecoder::destroyDecoder()
{
    if (packet_ != nullptr)
    {
        // The packet only ever borrows au_buffer_, so clear the borrow before
        // freeing it.
        packet_->data = nullptr;
        packet_->size = 0;
        av_packet_free(&packet_);
    }
    if (frame_ != nullptr)
    {
        av_frame_free(&frame_);
    }
    if (context_ != nullptr)
    {
        avcodec_free_context(&context_);
    }
    if (sws_ != nullptr)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
        sws_width_ = 0;
        sws_height_ = 0;
        sws_src_format_ = -1;
        sws_full_range_ = false;
    }
    synced_ = false;
    pending_config_.clear();
}

void VideoDecoder::reset()
{
    if (context_ != nullptr)
    {
        // Drops every reference frame the decoder is holding. Without this the
        // next GOP is decoded against pictures from the previous one, which
        // produces an image rather than an error -- so the failure looks like
        // corruption in the recording.
        avcodec_flush_buffers(context_);
    }
    synced_ = false;
    pending_config_.clear();

    // image_ is deliberately kept. A scrub would otherwise flash black between
    // releasing one GOP and decoding the next, at every render tick of a drag.
}

bool VideoDecoder::submit(const AccessUnit& unit)
{
    ++stats_.received;

    if (!ensureDecoder(unit.codec))
    {
        return false;
    }

    // Sync on either parameter sets or a keyframe. Keyframes are a valid entry
    // point because Annex-B access units carry SPS/PPS in band, and gating on
    // config alone leaves a reader that started mid-stream black indefinitely.
    if (unit.is_config || unit.is_keyframe)
    {
        synced_ = true;
    }
    else if (!synced_)
    {
        ++stats_.dropped_before_sync;
        return false;
    }

    // Parameter sets on their own are not a decodable access unit -- feeding
    // them straight to libavcodec yields AVERROR_INVALIDDATA. Cache them and
    // prepend to the next one instead; Annex-B concatenates freely.
    if (unit.is_config)
    {
        pending_config_.assign(unit.data.begin(), unit.data.end());
        return false;
    }

    if (unit.data.empty())
    {
        return false;
    }

    const std::size_t len = pending_config_.size() + unit.data.size();
    au_buffer_.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(au_buffer_.data(), pending_config_.data(), pending_config_.size());
    std::memcpy(au_buffer_.data() + pending_config_.size(), unit.data.data(), unit.data.size());
    std::memset(au_buffer_.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pending_config_.clear();

    // The packet borrows the padded buffer rather than allocating and copying:
    // send_packet does not take ownership, and libavcodec makes its own
    // reference if it needs the bytes beyond this call.
    packet_->data = au_buffer_.data();
    packet_->size = static_cast<int>(len);

    // THE SOURCE-CLOCK TIME RIDES THROUGH THE DECODER ON THE PACKET, and comes
    // back out on the frame it produced.
    //
    // The obvious alternative -- remember the time of the last access unit
    // submitted and stamp whatever comes out with it -- is correct only while
    // decode order is display order. That holds for CarPlay today (no B-frames,
    // delay = 0) but it is an assumption about the phone's encoder, not about
    // this code, and when it breaks the symptom is a frame labelled with another
    // frame's time. `frame_t` is the field a seek test asserts on, so a wrong
    // one turns the one check that could catch a bad seek into a check that
    // agrees with the bug.
    //
    // Microseconds, because pts is an integer and a double of seconds is not.
    packet_->pts = static_cast<std::int64_t>(std::llround(unit.t * 1e6));

    const int sent = avcodec_send_packet(context_, packet_);
    packet_->data = nullptr;
    packet_->size = 0;
    packet_->pts = AV_NOPTS_VALUE;

    if (sent < 0)
    {
        // Rate-limited: a persistent reject here means a black panel despite
        // being synced, so it has to be visible without --debug.
        if (++stats_.decode_errors % 60 == 1)
        {
            SPDLOG_WARN("[scope/video] decoder rejected {} packet(s) (last error {})",
                        stats_.decode_errors, sent);
        }
        return false;
    }

    // Fallback for a decoder that does not propagate the timestamp. Only used
    // when the frame comes back without one.
    pending_t_ = unit.t;

    bool produced = false;
    while (avcodec_receive_frame(context_, frame_) == 0)
    {
        if (!renderFrame(frame_))
        {
            if (++stats_.convert_errors % 60 == 1)
            {
                SPDLOG_WARN("[scope/video] cannot convert a decoded frame to RGB "
                            "({}x{}, pix_fmt {}); {} frame(s) dropped",
                            frame_->width, frame_->height, frame_->format,
                            stats_.convert_errors);
            }
            continue;
        }
        ++stats_.decoded;

        // best_effort_timestamp rather than pts: it falls back to the packet's
        // dts and to the decoder's own guess when a stream's timestamps are
        // partial, which is what "best effort" is for. AV_NOPTS_VALUE means it
        // had nothing at all to go on.
        const std::int64_t stamped = frame_->best_effort_timestamp;
        frame_t_ = (stamped == AV_NOPTS_VALUE) ? pending_t_
                                               : static_cast<double>(stamped) / 1e6;
        produced = true;
    }

    return produced;
}

bool VideoDecoder::renderFrame(const AVFrame* frame)
{
    if (frame == nullptr || frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return false;
    }

    const int w = frame->width;
    const int h = frame->height;

    // At the stream's own size, not the panel's. The CarPlay widget scales here
    // instead, and is right to: it is one fixed-size surface being touched, so
    // folding the resize into the colour conversion keeps paintEvent a blit.
    // A scope panel is dragged and re-docked constantly, and rebuilding the
    // swscale context on every frame of a resize costs more than the one
    // transform QPainter does at paint time.
    if (image_.width() != w || image_.height() != h ||
        image_.format() != QImage::Format_RGB32)
    {
        image_ = QImage(w, h, QImage::Format_RGB32);
        if (image_.isNull())
        {
            return false;
        }
    }

    // ffmpeg tags full-range 4:2:0 two different ways depending on decoder and
    // version: the deprecated YUVJ420P, or plain YUV420P with color_range set to
    // JPEG. Normalise to the latter -- swscale logs a deprecation warning for
    // the YUVJ formats, and it is the range flag that actually selects the
    // coefficients. Getting this wrong is washed-out colour, not an error.
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    bool full_range = (frame->color_range == AVCOL_RANGE_JPEG);
    if (src_fmt == AV_PIX_FMT_YUVJ420P)
    {
        src_fmt = AV_PIX_FMT_YUV420P;
        full_range = true;
    }
    else if (src_fmt == AV_PIX_FMT_YUVJ422P)
    {
        src_fmt = AV_PIX_FMT_YUV422P;
        full_range = true;
    }
    else if (src_fmt == AV_PIX_FMT_YUVJ444P)
    {
        src_fmt = AV_PIX_FMT_YUV444P;
        full_range = true;
    }

    // Rebuilt only when the geometry, format or range actually changes. SWS_POINT
    // because source and destination are the same size here, which selects
    // swscale's optimised unscaled YUV->RGB32 converter.
    if (sws_ == nullptr || w != sws_width_ || h != sws_height_ ||
        src_fmt != sws_src_format_ || full_range != sws_full_range_)
    {
        sws_ = sws_getCachedContext(sws_, w, h, src_fmt, w, h, kRgb32PixelFormat, SWS_POINT,
                                    nullptr, nullptr, nullptr);
        if (sws_ == nullptr)
        {
            return false;
        }

        int* inv_table = nullptr;
        int* table = nullptr;
        int src_range = 0;
        int dst_range = 0;
        int brightness = 0;
        int contrast = 0;
        int saturation = 0;
        if (sws_getColorspaceDetails(sws_, &inv_table, &src_range, &table, &dst_range,
                                     &brightness, &contrast, &saturation) >= 0)
        {
            sws_setColorspaceDetails(sws_, inv_table, full_range ? 1 : 0, table, dst_range,
                                     brightness, contrast, saturation);
        }

        sws_width_ = w;
        sws_height_ = h;
        sws_src_format_ = src_fmt;
        sws_full_range_ = full_range;
        SPDLOG_INFO("[scope/video] scaler ready: {}x{} {} -> RGB32 ({} range)", w, h,
                    av_get_pix_fmt_name(src_fmt), full_range ? "full" : "limited");
    }

    std::uint8_t* dst_planes[4] = {image_.bits(), nullptr, nullptr, nullptr};
    const int dst_strides[4] = {static_cast<int>(image_.bytesPerLine()), 0, 0, 0};
    return sws_scale(sws_, frame->data, frame->linesize, 0, h, dst_planes, dst_strides) > 0;
}

}  // namespace scope
