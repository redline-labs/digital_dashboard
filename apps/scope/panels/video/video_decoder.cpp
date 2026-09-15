#include "video/video_decoder.h"

#include "helpers/ffmpeg_log.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
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

// Slack on "is this frame at or before the instant asked for".
//
// A frame's time is not the number that went in. It rides through libavcodec on
// packet->pts, which is an integer, so it comes back quantised to microseconds
// and can land half a microsecond ABOVE the message time the caller is targeting.
// Compared exactly, such a frame reads as being in the future and the picture
// shown is the one before it -- a whole frame late, on a seek that asked for
// exactly the instant a frame exists at.
//
// Half a microsecond is far below anything a recording resolves and four orders
// of magnitude below a frame interval, so this cannot make a neighbouring frame
// eligible.
constexpr double kTargetEpsilon = 1e-6;

// Which GPU to ask, in order.
//
// One list rather than one decoder per API: libavcodec's hw_device_ctx path
// makes the difference between VideoToolbox and VAAPI a device type and a pixel
// format, and everything either side of it is the same code.
//
// Anything not listed is not refused, it is simply not looked for. A machine
// with no entry here decodes in software, which is the same thing that happens
// when a listed device is not present -- so this list is a preference, never a
// requirement.
constexpr AVHWDeviceType kPreferredHwDevices[] = {
#if defined(__APPLE__)
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
#endif
};

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

    // The GPU first, then the CPU. Each attempt cleans up after itself, so the
    // fallback starts from nothing rather than from half an open decoder.
    if (hw_enabled_ && !hw_failed_)
    {
        for (const AVHWDeviceType type : kPreferredHwDevices)
        {
            if (openWith(decoder, type))
            {
                codec_ = codec;
                SPDLOG_INFO("[scope/video] decoder ready ({} on {})",
                            (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264", backend_);
                return true;
            }
        }
    }

    if (!openWith(decoder, AV_HWDEVICE_TYPE_NONE))
    {
        SPDLOG_ERROR("[scope/video] failed to open the video decoder");
        destroyDecoder();
        return false;
    }

    codec_ = codec;
    SPDLOG_INFO("[scope/video] decoder ready ({} in software)",
                (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264");
    return true;
}

bool VideoDecoder::openWith(const AVCodec* decoder, int device_type)
{
    context_ = avcodec_alloc_context3(decoder);
    if (context_ == nullptr)
    {
        return false;
    }

    // How the get_format callback finds its way back here. It is a C function
    // pointer, so there is nowhere else to put the `this`.
    context_->opaque = this;

    if (device_type != AV_HWDEVICE_TYPE_NONE)
    {
        const auto type = static_cast<AVHWDeviceType>(device_type);

        // What pixel format this decoder produces on that device. A decoder with
        // no config for it cannot use it at all, whatever the machine has.
        AVPixelFormat hw_fmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i)
        {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (config == nullptr)
            {
                break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                config->device_type == type)
            {
                hw_fmt = config->pix_fmt;
                break;
            }
        }

        if (hw_fmt == AV_PIX_FMT_NONE ||
            av_hwdevice_ctx_create(&hw_device_, type, nullptr, nullptr, 0) < 0)
        {
            // No such device on this machine, which is the ordinary case on a
            // headless build host. Not a warning: software is a correct answer.
            SPDLOG_DEBUG("[scope/video] no {} device available",
                         av_hwdevice_get_type_name(type));
            destroyDecoder();
            return false;
        }

        context_->hw_device_ctx = av_buffer_ref(hw_device_);
        context_->get_format = &VideoDecoder::chooseFormat;
        hw_pix_fmt_ = hw_fmt;
        backend_ = av_hwdevice_get_type_name(type);
    }
    else
    {
        // THREADING IS CHOSEN BY WHAT THIS DECODER IS FOR, and the two answers
        // are opposite. Measured here on a 1280x720 stream, 120 access units,
        // decode only:
        //
        //     single thread    1.087 ms/frame   ->  65 ms per GOP catch-up
        //     slice threading  1.081 ms/frame   ->  65 ms  (buys nothing)
        //     frame threading  0.222 ms/frame   ->  13 ms
        //
        // Slice threading matching carplay_widget.cpp:136's measurement exactly:
        // the encoder sends one slice per picture, so there is nothing to split.
        //
        // Frame threading is five times faster and costs the one thing a live
        // panel cannot pay: it withholds thread_count - 1 pictures before
        // emitting the first, so the picture would sit four frames -- 133 ms --
        // behind the traces drawn beside it. For a scope that is not latency,
        // it is the panel failing at the thing it exists for.
        //
        // Over a RECORDING there is nothing to be behind. The catch-up is the
        // whole cost, the withheld pictures are handled by the target model and
        // drain(), and five times faster is exactly what a scrub wants.
        if (seek_optimised_)
        {
            context_->thread_count = 0;
            context_->thread_type = FF_THREAD_FRAME;
        }
        else
        {
            context_->thread_count = 1;
        }
        backend_ = "software";
    }

    if (avcodec_open2(context_, decoder, nullptr) < 0)
    {
        destroyDecoder();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    sw_frame_ = av_frame_alloc();
    decoded_on_context_ = 0;
    synced_ = false;
    return frame_ != nullptr && packet_ != nullptr && sw_frame_ != nullptr;
}

AVPixelFormat VideoDecoder::chooseFormat(AVCodecContext* context, const AVPixelFormat* formats)
{
    auto* self = static_cast<VideoDecoder*>(context->opaque);

    for (const AVPixelFormat* p = formats; p != nullptr && *p != AV_PIX_FMT_NONE; ++p)
    {
        if (self != nullptr && *p == self->hw_pix_fmt_)
        {
            return *p;
        }
    }

    // The device cannot do this stream -- an unsupported profile is the usual
    // reason. libavcodec will happily decode it in software from here, which is
    // the right outcome and the wrong thing to be silent about: hw_pix_fmt_ is
    // cleared so nothing downstream goes looking for a surface that will never
    // arrive, and backend() stops claiming the GPU.
    if (self != nullptr && self->hw_pix_fmt_ != AV_PIX_FMT_NONE)
    {
        SPDLOG_INFO("[scope/video] {} cannot decode this stream; using software",
                    self->backend_);
        self->hw_pix_fmt_ = AV_PIX_FMT_NONE;
        self->hw_failed_ = true;
        self->backend_ = "software";
    }

    return (formats != nullptr) ? formats[0] : AV_PIX_FMT_NONE;
}

bool VideoDecoder::hardware() const
{
    return hw_pix_fmt_ != AV_PIX_FMT_NONE;
}

void VideoDecoder::setHardwareEnabled(bool on)
{
    if (on == hw_enabled_)
    {
        return;
    }
    hw_enabled_ = on;

    // CLOSES THE DECODER, so the change takes effect rather than waiting for a
    // codec switch that may never come. A backend cannot be changed under an
    // open context, and a knob whose whole purpose is "the GPU path is the
    // suspect, take it away" is useless if it only applies to some later stream.
    //
    // The caller has to re-feed from a keyframe afterwards -- destroyDecoder()
    // leaves this un-synced, so anything before the next one is dropped rather
    // than fed to a decoder holding no references.
    destroyDecoder();
    hw_failed_ = false;
}

void VideoDecoder::setSeekOptimised(bool on)
{
    if (on == seek_optimised_)
    {
        return;
    }
    seek_optimised_ = on;

    // Threading cannot be changed under an open context, so this closes it. The
    // caller feeds from a keyframe again afterwards -- which is what a source
    // change, the only thing that flips this, does anyway.
    destroyDecoder();
}

bool VideoDecoder::fallBackToSoftware()
{
    if (hw_pix_fmt_ == AV_PIX_FMT_NONE)
    {
        return false;
    }

    SPDLOG_WARN("[scope/video] {} decode failed; falling back to software", backend_);
    hw_failed_ = true;

    const Codec codec = codec_;
    destroyDecoder();

    // Reopening leaves the decoder un-synced, so everything until the next
    // keyframe is dropped rather than fed to a decoder with no references --
    // the same state a subscriber that joined mid-stream is in, and it recovers
    // the same way.
    if (!ensureDecoder(codec))
    {
        return false;
    }
    return true;
}

void VideoDecoder::destroyDecoder()
{
    // BEFORE the context goes. A held frame references buffers the codec
    // allocated, and releasing them while the decoder that owns the pool is
    // still alive is the ordering libavcodec documents.
    clearHeldFrames();

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
    if (sw_frame_ != nullptr)
    {
        av_frame_free(&sw_frame_);
    }
    if (context_ != nullptr)
    {
        avcodec_free_context(&context_);
    }
    if (hw_device_ != nullptr)
    {
        // AFTER the context, which holds its own reference to this. Releasing
        // ours first is harmless because of that, but doing it in this order
        // means the device outlives every frame that came off it.
        av_buffer_unref(&hw_device_);
    }
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    backend_ = "software";
    decoded_on_context_ = 0;
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

    // Every held frame belongs to the GOP being left behind, including the ones
    // stashed ahead of the target. Kept, they would be presented against the new
    // position -- a picture from the old place, stamped with a time that says it
    // is from the new one.
    clearHeldFrames();

    // image_ is deliberately kept. A scrub would otherwise flash black between
    // releasing one GOP and decoding the next, at every render tick of a drag.
}

void VideoDecoder::clearImage()
{
    // For a change of SOURCE, where keeping the picture is the wrong call: the
    // times either side of the move are on different epochs, so the frame from
    // the old one can collide with an instant in the new and never be redrawn.
    image_ = QImage();
    frame_t_ = 0.0;
    has_presented_ = false;
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
        // A HARDWARE DECODER THAT HAS NEVER PRODUCED ANYTHING is not rejecting a
        // bad access unit, it is refusing the stream -- a profile the GPU does
        // not implement, or a device that came up but cannot be used. Software
        // is slower and works. Once it HAS decoded, a rejected packet is a
        // damaged one and falling back would be blaming the wrong thing.
        if (hardware() && decoded_on_context_ == 0 && fallBackToSoftware())
        {
            return false;
        }

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

    return collectFrames();
}

// --------------------------------------------------------------- presenting

bool VideoDecoder::collectFrames()
{
    bool changed = false;
    while (context_ != nullptr && avcodec_receive_frame(context_, frame_) == 0)
    {
        ++stats_.decoded;
        ++decoded_on_context_;

        // best_effort_timestamp rather than pts: it falls back to the packet's
        // dts and to the decoder's own guess when a stream's timestamps are
        // partial, which is what "best effort" is for. AV_NOPTS_VALUE means it
        // had nothing at all to go on.
        const std::int64_t stamped = frame_->best_effort_timestamp;
        const double t =
            (stamped == AV_NOPTS_VALUE) ? pending_t_ : static_cast<double>(stamped) / 1e6;

        changed = keepFrame(frame_, t) || changed;

        // frame_ is REUSED by the next receive_frame, so whatever keepFrame
        // decided to hold on to it took a reference to. Dropping ours here is
        // what makes that reference the only one.
        av_frame_unref(frame_);
    }
    return changed;
}

bool VideoDecoder::keepFrame(AVFrame* frame, double t)
{
    if (t > target_ + kTargetEpsilon)
    {
        // After the instant asked for. Kept rather than dropped, because the
        // target moves forward and libavcodec will not emit this frame twice --
        // see future_.
        reached_ = true;
        if (future_.size() < kMaxFutureFrames)
        {
            AVFrame* held = av_frame_alloc();
            if (held != nullptr && av_frame_ref(held, frame) == 0)
            {
                future_.push_back(HeldFrame{held, t});
            }
            else if (held != nullptr)
            {
                av_frame_free(&held);
            }
        }
        return false;
    }

    // At or before the target. The LATEST such frame is the one being asked
    // for, so an earlier one arriving after it -- which reordering can do --
    // must not displace it.
    if (candidate_ != nullptr && t <= candidate_t_)
    {
        return false;
    }

    AVFrame* held = av_frame_alloc();
    if (held == nullptr || av_frame_ref(held, frame) != 0)
    {
        if (held != nullptr)
        {
            av_frame_free(&held);
        }
        return false;
    }

    if (candidate_ != nullptr)
    {
        av_frame_free(&candidate_);
    }
    candidate_ = held;
    candidate_t_ = t;
    return true;
}

bool VideoDecoder::promoteFutureFrames()
{
    bool changed = false;
    reached_ = false;

    for (std::size_t i = 0; i < future_.size();)
    {
        HeldFrame held = future_[i];

        if (held.t > target_ + kTargetEpsilon)
        {
            reached_ = true;
            ++i;
            continue;
        }

        if (candidate_ == nullptr || held.t > candidate_t_)
        {
            if (candidate_ != nullptr)
            {
                av_frame_free(&candidate_);
            }
            candidate_ = held.frame;
            candidate_t_ = held.t;
            changed = true;
        }
        else
        {
            av_frame_free(&held.frame);
        }
        future_.erase(future_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    return changed;
}

void VideoDecoder::clearHeldFrames()
{
    if (candidate_ != nullptr)
    {
        av_frame_free(&candidate_);
    }
    candidate_t_ = 0.0;
    for (HeldFrame& held : future_)
    {
        av_frame_free(&held.frame);
    }
    future_.clear();
    reached_ = false;
}

void VideoDecoder::setTarget(double t)
{
    if (t == target_)
    {
        return;
    }

    const bool backwards = t < target_;
    target_ = t;

    if (backwards && candidate_ != nullptr && candidate_t_ > target_ + kTargetEpsilon)
    {
        // The candidate is now in the future. It is still a decoded picture and
        // the target may come back to it, so it goes to the stash rather than
        // the bin -- but it must stop being what present() would draw, or a
        // backwards seek would keep showing the frame from before it.
        if (future_.size() < kMaxFutureFrames)
        {
            future_.push_back(HeldFrame{candidate_, candidate_t_});
        }
        else
        {
            av_frame_free(&candidate_);
        }
        candidate_ = nullptr;
        candidate_t_ = 0.0;
    }

    promoteFutureFrames();
}

bool VideoDecoder::drain()
{
    if (context_ == nullptr)
    {
        return false;
    }

    // A null packet asks libavcodec for everything it is holding. The decoder
    // ends up drained, so flush it afterwards to make it usable again -- without
    // that, the next send_packet returns AVERROR_EOF and the panel goes black
    // one frame after every seek that ended at a window's edge.
    avcodec_send_packet(context_, nullptr);
    const bool changed = collectFrames();
    avcodec_flush_buffers(context_);
    return changed;
}

bool VideoDecoder::present()
{
    if (candidate_ == nullptr)
    {
        return false;
    }
    if (has_presented_ && candidate_t_ == frame_t_)
    {
        // Already on screen. A scrub that moves within one frame's worth of time
        // asks for the same picture repeatedly, and converting it again would
        // pay the whole cost of a repaint to produce identical pixels.
        return false;
    }

    if (!renderFrame(candidate_))
    {
        if (++stats_.convert_errors % 60 == 1)
        {
            SPDLOG_WARN("[scope/video] cannot convert a decoded frame to RGB "
                        "({}x{}, pix_fmt {}); {} frame(s) dropped",
                        candidate_->width, candidate_->height, candidate_->format,
                        stats_.convert_errors);
        }
        return false;
    }

    frame_t_ = candidate_t_;
    has_presented_ = true;
    ++stats_.presented;
    return true;
}

bool VideoDecoder::renderFrame(AVFrame* frame)
{
    if (frame == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return false;
    }

    // BEFORE THE data[0] CHECK BELOW, and that ordering is load-bearing. A
    // hardware frame carries its surface handle in a plane that depends on the
    // API -- data[3] for VideoToolbox and VAAPI -- and data[0] is null. Testing
    // it first rejects every frame the GPU produces, which looks exactly like a
    // decoder that cannot decode the stream.
    if (hw_pix_fmt_ != AV_PIX_FMT_NONE && frame->format == hw_pix_fmt_)
    {
        // THE COPY BACK FROM THE GPU, and the only place it happens.
        //
        // A hardware frame is a handle to a surface in video memory, not pixels
        // this process can read. Everything below needs pixels, so the picture
        // crosses the bus here -- which is why this is in present() rather than
        // in the decode loop: the frames of a catch-up are never presented and
        // therefore never cross it at all.
        av_frame_unref(sw_frame_);

        // Left as AV_PIX_FMT_NONE so ffmpeg picks the format the device
        // transfers most cheaply (NV12 for VideoToolbox). Naming one here would
        // ask the driver for a conversion swscale does better below.
        sw_frame_->format = AV_PIX_FMT_NONE;

        if (av_hwframe_transfer_data(sw_frame_, frame, 0) < 0)
        {
            return false;
        }

        // Colour range, timestamps and the rest do NOT come across with the
        // pixels. Without this the frame arrives untagged and is converted with
        // the wrong coefficients -- washed-out colour, not an error.
        av_frame_copy_props(sw_frame_, frame);
        frame = sw_frame_;
    }

    if (frame->data[0] == nullptr)
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
