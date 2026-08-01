#include <cstdlib>
#include "carplay/carplay_widget.h"

#include <spdlog/spdlog.h>
#include <QPainter>
#include <QTimer>
#include <QtGui/QResizeEvent>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QAudioSource>
#include <QtMultimedia/QMediaDevices>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace
{
// QImage::Format_RGB32 packs 0xffRRGGBB into a uint32, so the byte order in
// memory follows the host endianness. Matching it exactly is the whole point:
// it makes drawImage a straight blit instead of a per-pixel conversion.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
constexpr AVPixelFormat kRgb32PixelFormat = AV_PIX_FMT_BGRA;
#else
constexpr AVPixelFormat kRgb32PixelFormat = AV_PIX_FMT_ARGB;
#endif

// Ceiling on touch reports sent to the phone during a drag.
//
// 60, not 30: the phone derives scroll momentum from the last few samples of a
// gesture, and at 30 Hz a quick flick only lands two or three of them, which
// makes the fling velocity it computes noisy. The tell is taps and slow drags
// feeling fine while flicks come out inconsistent. We also advertise
// high-fidelity touch in the AirPlay /info payload, so 30 would undersell what
// we claim. Against a 500-1000 Hz gaming mouse this is still a 8-16x cut, and
// it costs twice what 30 would.
constexpr int kTouchPublishHz = 60;
constexpr auto kTouchMinInterval = std::chrono::microseconds(1'000'000 / kTouchPublishHz);
}  // namespace

CarPlayWidget::CarPlayWidget(CarplayConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg)),
    _touch_throttle(kTouchMinInterval)
{
    setAttribute(Qt::WA_OpaquePaintEvent);

    // Seed the scaler target so it is never zero. Whatever size we have now is
    // provisional -- the layout sets real geometry after construction, which
    // fires resizeEvent and republishes -- but frames can only arrive once the
    // subscribers below exist, by which point this has been corrected.
    publishTargetSize();

    // Precise rather than coarse: this paces touch, and Qt's default 5% slop
    // is enough to make the interval visibly uneven at 16 ms.
    _touch_flush_timer = new QTimer(this);
    _touch_flush_timer->setSingleShot(true);
    _touch_flush_timer->setTimerType(Qt::PreciseTimer);
    connect(_touch_flush_timer, &QTimer::timeout, this, [this] {
        const auto pending = _touch_throttle.takePending(std::chrono::steady_clock::now());
        if (pending.has_value())
        {
            publishInput(CarPlayInput::Kind::TOUCH_MOVE, QPointF(pending->x, pending->y));
        }
    });

    _input_pub = std::make_unique<pub_sub::ZenohPublisher<CarPlayInput>>(_cfg.input_key);

    _video_sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayVideo>>(
        _cfg.video_key,
        [this](CarPlayVideo::Reader reader) { onVideoMessage(reader); });

    _audio_sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlayAudio>>(
        _cfg.audio_key,
        [this](CarPlayAudio::Reader reader) { onAudioMessage(reader); });

    _session_sub = std::make_unique<pub_sub::ZenohTypedSubscriber<CarPlaySessionState>>(
        _cfg.session_key,
        [this](CarPlaySessionState::Reader reader) { onSessionMessage(reader); });

    _mic_pub = std::make_unique<pub_sub::ZenohPublisher<CarPlayAudio>>(_cfg.mic_key);
}

CarPlayWidget::~CarPlayWidget()
{
    // Drop the subscribers first so their callbacks cannot race teardown.
    _video_sub.reset();
    _audio_sub.reset();
    _session_sub.reset();
    // Stop the sink before the ring it pulls from (member destruction order is
    // the reverse of declaration, which would free the ring first).
    _audio_sink.reset();
    _audio_ring.reset();
    stopMicrophone();
    destroyDecoder();
}

bool CarPlayWidget::ensureDecoder(CarPlayVideo::Codec codec)
{
    if (_codec_context != nullptr && codec == _codec_id)
    {
        return true;
    }
    destroyDecoder();

    const AVCodecID av_id = (codec == CarPlayVideo::Codec::H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* decoder = avcodec_find_decoder(av_id);
    if (decoder == nullptr)
    {
        SPDLOG_ERROR("No libavcodec decoder for {}", (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264");
        return false;
    }

    _codec_context = avcodec_alloc_context3(decoder);
    // Threading is deliberately left alone, which means thread_count = 1.
    // Measured on a real 800x600 capture (928 pictures, 4-core host): the phone
    // sends exactly 1.00 slices per picture, so FF_THREAD_SLICE has nothing to
    // parallelise and buys nothing -- 1.078 vs 1.075 ms/frame, i.e. slightly
    // worse once thread sync is paid for.
    //
    // Setting thread_count = 0 *is* faster (0.47 ms/frame) but it selects frame
    // threading, which withholds thread_count - 1 pictures before emitting the
    // first: 4 frames, ~133 ms at 30 fps, on a screen the user is touching. The
    // stream carries no B-frames and the decoder reports delay = 0 today, so
    // right now we add no decode latency at all -- worth keeping.
    //
    // Decode already costs ~3.2% of one core, so there is no headroom to win
    // and real latency to lose. Revisit only if the panel resolution grows a
    // lot; re-measure by capturing a stream with AIRPLAY_DUMP_VIDEO (see
    // libs/airplay/receiver.cpp) rather than reasoning from defaults.
    if (_codec_context == nullptr || avcodec_open2(_codec_context, decoder, nullptr) < 0)
    {
        SPDLOG_ERROR("Failed to open video decoder");
        destroyDecoder();
        return false;
    }

    _frame = av_frame_alloc();
    _pkt = av_packet_alloc();
    _codec_id = codec;
    _synced = false;
    SPDLOG_INFO("CarPlay video decoder ready ({})", (av_id == AV_CODEC_ID_HEVC) ? "HEVC" : "H.264");
    return true;
}

void CarPlayWidget::destroyDecoder()
{
    if (_pkt != nullptr)
    {
        // _pkt only ever borrows _au_buf, so clear the borrow before freeing.
        _pkt->data = nullptr;
        _pkt->size = 0;
        av_packet_free(&_pkt);
    }
    if (_frame != nullptr) av_frame_free(&_frame);
    if (_codec_context != nullptr) avcodec_free_context(&_codec_context);
    if (_sws != nullptr)
    {
        sws_freeContext(_sws);
        _sws = nullptr;
        _sws_width = 0;
        _sws_height = 0;
        _sws_dst_width = 0;
        _sws_dst_height = 0;
        _sws_src_format = -1;
        _sws_full_range = false;
    }
    _synced = false;
}

void CarPlayWidget::onVideoMessage(CarPlayVideo::Reader reader)
{
    if (!ensureDecoder(reader.getCodec()))
    {
        return;
    }

    const uint32_t seq = reader.getSeq();
    if (_last_seq != 0 && seq != _last_seq + 1)
    {
        SPDLOG_WARN("[carplay] video sequence jump {} -> {}, resyncing", _last_seq, seq);
        // Wait for a fresh sync point before feeding the decoder again so it
        // does not smear across the gap.
        _synced = false;
    }
    _last_seq = seq;

    // Sync on either a parameter-set message or a keyframe. Keyframes are a
    // valid entry point because Annex-B access units carry SPS/PPS in band,
    // and config is published once per session -- gating on config alone
    // leaves a late-joining or restarted widget black forever.
    if (reader.getIsConfig() || reader.getIsKeyframe())
    {
        if (!_synced)
        {
            SPDLOG_INFO("[carplay] video synced on {} (seq {})",
                        reader.getIsConfig() ? "parameter sets" : "keyframe", seq);
        }
        _synced = true;
    }
    else if (!_synced)
    {
        // Loud enough to diagnose a black screen, quiet enough not to spam.
        if (++_dropped_before_sync % 120 == 1)
        {
            SPDLOG_WARN("[carplay] dropped {} frame(s) waiting for a keyframe/config to sync on",
                        _dropped_before_sync);
        }
        return;
    }

    // Not named `data`: QWidget has a member by that name and -Wshadow flags it.
    auto payload = reader.getData();

    // Parameter sets on their own are not a decodable access unit -- feeding
    // them straight to libavcodec yields AVERROR_INVALIDDATA. Cache them and
    // prepend to the next access unit instead (Annex-B concatenates freely).
    if (reader.getIsConfig())
    {
        _pending_config.assign(payload.begin(), payload.end());
        return;
    }

    // Assemble [pending parameter sets][access unit] into the reusable buffer.
    // libavcodec reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end of
    // a packet it does not own, so the tail must exist and be zeroed.
    const size_t len = _pending_config.size() + payload.size();
    _au_buf.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(_au_buf.data(), _pending_config.data(), _pending_config.size());
    std::memcpy(_au_buf.data() + _pending_config.size(), payload.begin(), payload.size());
    std::memset(_au_buf.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    _pending_config.clear();

    decodeAccessUnit(_au_buf.data(), len);
}

// The parameter is not called `data`: QWidget has a `data` member, and shadowing
// it trips -Wshadow.
void CarPlayWidget::decodeAccessUnit(const uint8_t* annexb, size_t len)
{
    if (len == 0)
    {
        return;
    }

    // The driver publishes whole Annex-B access units, so no parser is needed.
    // The packet borrows the caller's padded buffer rather than allocating and
    // copying: send_packet does not take ownership, and libavcodec makes its own
    // reference if it needs the bytes beyond this call.
    _pkt->data = const_cast<uint8_t*>(annexb);
    _pkt->size = static_cast<int>(len);

    int ret = avcodec_send_packet(_codec_context, _pkt);
    _pkt->data = nullptr;
    _pkt->size = 0;
    if (ret < 0)
    {
        // Rate-limited: a persistent reject here means a black screen despite
        // being synced, so it must be visible without --verbose.
        if (++_decode_errors % 60 == 1)
        {
            SPDLOG_WARN("[carplay] decoder rejected {} packet(s) (last error {})", _decode_errors, ret);
        }
        return;
    }

    bool frame_updated = false;
    while (avcodec_receive_frame(_codec_context, _frame) == 0)
    {
        if (!renderFrameToBackBuffer(_frame))
        {
            // Anything swscale cannot handle lands here and would render black.
            if (++_convert_errors % 60 == 1)
            {
                SPDLOG_WARN("[carplay] cannot convert decoded frame to RGB ({}x{}, pix_fmt {}); "
                            "{} frame(s) dropped",
                            _frame->width, _frame->height, _frame->format, _convert_errors);
            }
            continue;
        }
        frame_updated = true;

        if (!_rendered_first_frame)
        {
            _rendered_first_frame = true;
            SPDLOG_INFO("[carplay] first video frame decoded and rendered ({}x{})",
                        _frame->width, _frame->height);
            // Bring-up aid: dump the first rendered frame so it can be inspected
            // without a screenshot tool. Grabs the exact QImage the widget draws.
            if (const char* path = std::getenv("CARPLAY_DUMP_RENDER"); path != nullptr)
            {
                std::lock_guard<std::mutex> lock(_frame_mutex);
                if (_front_frame >= 0 && _frames[_front_frame].save(QString::fromUtf8(path)))
                {
                    SPDLOG_INFO("[carplay] wrote rendered frame to {}", path);
                }
            }
        }
    }

    if (frame_updated)
    {
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }
}

bool CarPlayWidget::renderFrameToBackBuffer(const AVFrame* frame)
{
    // CarPlay decodes to YUVJ420P (full-range, pix_fmt 12); a synthetic or
    // other source may give plain YUV420P (limited range, pix_fmt 0). swscale
    // handles both -- and, unlike the hand-rolled loop this replaces, applies
    // the range each one actually declares rather than assuming full range.
    if (frame == nullptr || frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return false;
    }
    const int w = frame->width;
    const int h = frame->height;

    // Scale to the widget rather than to the stream, so paintEvent is always a
    // straight blit. swscale folds the resize into the colour conversion it has
    // to do anyway -- one pass over the pixels either way -- whereas leaving the
    // resize to drawImage(rect(), img) costs a second full transform pass, and
    // costs it on the GUI thread on *every* repaint, not once per decoded frame.
    // That distinction matters as soon as overlays sit on top of the video and
    // repaint independently of the frame rate.
    const uint64_t target = _target_size.load(std::memory_order_relaxed);
    const int dst_w = (target != 0) ? static_cast<int>(target >> 32) : w;
    const int dst_h = (target != 0) ? static_cast<int>(target & 0xffffffffu) : h;
    if (dst_w <= 0 || dst_h <= 0)
    {
        return false;
    }

    // Format_RGB32 is Qt's native raster format, so paintEvent blits it instead
    // of converting per pixel. Reused across frames: the allocation only
    // happens on the first frame and on a resolution change.
    QImage& dst = _frames[_back_frame];
    if (dst.width() != dst_w || dst.height() != dst_h || dst.format() != QImage::Format_RGB32)
    {
        dst = QImage(dst_w, dst_h, QImage::Format_RGB32);
        if (dst.isNull())
        {
            return false;
        }
    }

    // ffmpeg tags full-range 4:2:0 two different ways depending on decoder and
    // version: the deprecated YUVJ420P, or plain YUV420P with color_range set
    // to JPEG. Normalise to the latter -- swscale logs a deprecation warning
    // for the YUVJ formats, and it is the range flag that actually selects the
    // coefficients. Output is bit-identical to passing YUVJ420P through.
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

    // Rebuilt only when the geometry, format or range actually changes; steady
    // state reuses the context. SWS_POINT when the sizes match, which selects
    // swscale's optimised unscaled YUV->RGB32 converter; bilinear when a real
    // resize is needed, since a nearest-neighbour resample of video is visibly
    // worse and costs about the same inside a pass that is memory bound anyway.
    if (_sws == nullptr || w != _sws_width || h != _sws_height || dst_w != _sws_dst_width ||
        dst_h != _sws_dst_height || src_fmt != _sws_src_format || full_range != _sws_full_range)
    {
        const bool unscaled = (w == dst_w && h == dst_h);
        _sws = sws_getCachedContext(_sws,
                                    w, h, src_fmt,
                                    dst_w, dst_h, kRgb32PixelFormat,
                                    unscaled ? SWS_POINT : SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
        if (_sws == nullptr)
        {
            return false;
        }

        int* inv_table = nullptr;
        int* table = nullptr;
        int src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
        if (sws_getColorspaceDetails(_sws, &inv_table, &src_range, &table, &dst_range,
                                     &brightness, &contrast, &saturation) >= 0)
        {
            sws_setColorspaceDetails(_sws, inv_table, full_range ? 1 : 0, table, dst_range,
                                     brightness, contrast, saturation);
        }

        _sws_width = w;
        _sws_height = h;
        _sws_dst_width = dst_w;
        _sws_dst_height = dst_h;
        _sws_src_format = src_fmt;
        _sws_full_range = full_range;
        SPDLOG_INFO("[carplay] video scaler ready: {}x{} {} -> {}x{} RGB32 ({} range)",
                    w, h, av_get_pix_fmt_name(src_fmt), dst_w, dst_h,
                    full_range ? "full" : "limited");
    }

    uint8_t* dst_planes[4] = {dst.bits(), nullptr, nullptr, nullptr};
    const int dst_strides[4] = {static_cast<int>(dst.bytesPerLine()), 0, 0, 0};
    if (sws_scale(_sws, frame->data, frame->linesize, 0, h, dst_planes, dst_strides) <= 0)
    {
        return false;
    }

    // Publish. Only the index moves under the lock -- no pixels are copied, and
    // paintEvent cannot be mid-blit on this buffer because it holds the lock
    // while it draws.
    std::lock_guard<std::mutex> lock(_frame_mutex);
    _front_frame = _back_frame;
    _back_frame ^= 1;
    return true;
}

void CarPlayWidget::onAudioMessage(CarPlayAudio::Reader reader)
{
    auto pcm = reader.getPcm();
    if (pcm.size() == 0)
    {
        return;
    }
    const int rate = static_cast<int>(reader.getSampleRateHz());
    const int channels = reader.getChannels();
    if (rate <= 0 || channels <= 0)
    {
        return;
    }

    // (Re)build the sink on the GUI thread when the format changes -- but only
    // then. In steady state we push straight into the thread-safe ring from
    // this (subscriber) thread, with no GUI-thread hop per packet.
    if (rate != _sink_sample_rate.load() || channels != _sink_channels.load())
    {
        ensureAudioSink(rate, channels);
    }
    if (_audio_ring)
    {
        _audio_ring->push(reinterpret_cast<const char*>(pcm.begin()),
                          static_cast<qint64>(pcm.size()));
    }
}

void CarPlayWidget::ensureAudioSink(int sample_rate, int channels)
{
    // QAudioSink lives on the GUI thread; recreate it there. Block so the ring
    // exists before we return to the caller that is about to push into it.
    QMetaObject::invokeMethod(
        this,
        [this, sample_rate, channels] {
            QAudioFormat format;
            format.setSampleRate(sample_rate);
            format.setChannelCount(channels);
            format.setSampleFormat(QAudioFormat::Int16);

            const QAudioDevice device = QMediaDevices::defaultAudioOutput();
            if (!device.isFormatSupported(format))
            {
                SPDLOG_WARN("[carplay] audio format {} Hz / {} ch unsupported by '{}'; using its "
                            "preferred format",
                            sample_rate, channels, device.description().toStdString());
                format = device.preferredFormat();
            }

            auto ring = std::make_unique<AudioRingBuffer>();
            ring->configure(format.sampleRate(), format.channelCount());
            ring->open(QIODevice::ReadOnly);

            auto sink = std::make_unique<QAudioSink>(device, format);
            sink->start(ring.get());  // pull mode: the sink drains the ring

            // Stop the old sink before freeing the old ring, or its audio thread
            // pulls from freed memory. ~QAudioSink joins the audio thread.
            _audio_sink.reset();
            _audio_ring = std::move(ring);
            _audio_sink = std::move(sink);
            _sink_sample_rate.store(sample_rate);
            _sink_channels.store(channels);
            SPDLOG_INFO("[carplay] audio sink started (pull mode): {} Hz / {} ch on '{}'",
                        format.sampleRate(), format.channelCount(),
                        device.description().toStdString());
        },
        Qt::BlockingQueuedConnection);
}

void CarPlayWidget::startMicrophone(int sample_rate, int channels)
{
    if (_mic_source != nullptr && sample_rate == _mic_sample_rate && channels == _mic_channels)
    {
        return;
    }
    stopMicrophone();

    QAudioFormat format;
    format.setSampleRate(sample_rate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!device.isFormatSupported(format))
    {
        SPDLOG_WARN("[carplay] mic format {} Hz / {} ch unsupported; using preferred", sample_rate, channels);
        format = device.preferredFormat();
    }

    _mic_source = std::make_unique<QAudioSource>(device, format);
    _mic_device = _mic_source->start();
    if (_mic_device == nullptr)
    {
        SPDLOG_ERROR("[carplay] failed to start microphone capture");
        _mic_source.reset();
        return;
    }
    _mic_sample_rate = format.sampleRate();
    _mic_channels = format.channelCount();
    connect(_mic_device, &QIODevice::readyRead, this, &CarPlayWidget::pumpMicrophone);
    SPDLOG_INFO("[carplay] microphone started: {} Hz / {} ch on '{}'",
                _mic_sample_rate, _mic_channels, device.description().toStdString());
}

void CarPlayWidget::stopMicrophone()
{
    if (_mic_source != nullptr)
    {
        _mic_source->stop();
        _mic_source.reset();
        _mic_device = nullptr;
        _mic_sample_rate = 0;
        _mic_channels = 0;
        SPDLOG_INFO("[carplay] microphone stopped");
    }
}

void CarPlayWidget::pumpMicrophone()
{
    if (_mic_device == nullptr || _mic_pub == nullptr)
    {
        return;
    }
    const QByteArray chunk = _mic_device->readAll();
    if (chunk.isEmpty())
    {
        return;
    }

    auto& fields = _mic_pub->fields();
    fields.setSampleRateHz(static_cast<uint32_t>(_mic_sample_rate));
    fields.setChannels(static_cast<uint8_t>(_mic_channels));
    fields.setStreamType(CarPlayAudio::StreamType::MIC);
    fields.setPcm(kj::arrayPtr(reinterpret_cast<const uint8_t*>(chunk.constData()),
                               static_cast<size_t>(chunk.size())));
    _mic_pub->put();
}

void CarPlayWidget::onSessionMessage(CarPlaySessionState::Reader reader)
{
    std::string status;
    if (!reader.getDeviceConnected())
    {
        status = "Connect an iPhone";
    }
    else if (reader.getPhase() != CarPlaySessionState::Phase::RECORDING)
    {
        status = "Connecting...";
    }

    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        _status_text = std::move(status);
    }

    // Microphone follows the driver's request; Qt Multimedia objects live on
    // the GUI thread, so hop there.
    const bool mic_active = reader.getMicActive();
    const int mic_rate = static_cast<int>(reader.getMicSampleRateHz());
    const int mic_channels = reader.getMicChannels();
    QMetaObject::invokeMethod(
        this,
        [this, mic_active, mic_rate, mic_channels] {
            if (mic_active && mic_rate > 0 && mic_channels > 0)
            {
                startMicrophone(mic_rate, mic_channels);
            }
            else
            {
                stopMicrophone();
            }
            update();
        },
        Qt::QueuedConnection);
}

void CarPlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);

    // Held across the blit, not just the read: the buffer belongs to the
    // decoder's ping-pong pair, and this lock is what keeps it from being
    // swapped and overwritten while QPainter is reading it.
    std::unique_lock<std::mutex> lock(_frame_mutex);

    if (_front_frame >= 0)
    {
        const QImage& img = _frames[_front_frame];
        if (img.size() == size())
        {
            // The steady-state path: swscale already produced the frame at
            // widget size in Format_RGB32, so this is a straight blit with no
            // scale and no per-pixel conversion.
            p.drawImage(0, 0, img);
        }
        else
        {
            // Only reachable for the one frame between a resize and the next
            // decode, and while the widget still shows a frame from a stream
            // whose size just changed. Stretches, as it always has.
            p.drawImage(rect(), img);
        }
        return;
    }

    const std::string status = _status_text;
    lock.unlock();

    p.fillRect(rect(), Qt::black);
    if (!status.empty())
    {
        p.setPen(QColor(0x88, 0x88, 0x88));
        p.drawText(rect(), Qt::AlignCenter, QString::fromStdString(status));
    }
}

void CarPlayWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    publishTargetSize();
}

void CarPlayWidget::publishTargetSize()
{
    const uint64_t packed = (static_cast<uint64_t>(std::max(0, width())) << 32) |
                            static_cast<uint32_t>(std::max(0, height()));
    // The decode thread picks this up on its next frame. A resize that lands
    // mid-conversion just means one frame is converted at the old size and then
    // stretched by paintEvent; the frame after it is correct.
    _target_size.store(packed, std::memory_order_relaxed);
}

void CarPlayWidget::publishInput(CarPlayInput::Kind kind, const QPointF& pos)
{
    if (_input_pub == nullptr || width() <= 0 || height() <= 0)
    {
        return;
    }

    const auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    auto& fields = _input_pub->fields();
    fields.setKind(kind);
    fields.setX(static_cast<uint16_t>(clamp01(pos.x() / width()) * 10000.0));
    fields.setY(static_cast<uint16_t>(clamp01(pos.y() / height()) * 10000.0));
    _input_pub->put();
}

void CarPlayWidget::publishTouchMove(const QPointF& pos)
{
    const auto decision =
        _touch_throttle.onMove(toThrottlePoint(pos), std::chrono::steady_clock::now());
    if (decision.action == TouchThrottle::Action::Publish)
    {
        _touch_flush_timer->stop();
        publishInput(CarPlayInput::Kind::TOUCH_MOVE, pos);
        return;
    }

    // Deferred. The throttle is already holding the position; all that is left
    // is making sure something will come back for it. Arming once is enough --
    // later moves in the same interval only overwrite what it will send.
    if (!_touch_flush_timer->isActive())
    {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(decision.wait);
        _touch_flush_timer->start(std::max<int>(1, static_cast<int>(remaining.count())));
    }
}

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    _touch_active = true;
    // Drops any motion pending from the previous gesture, which must never land
    // after this down.
    _touch_flush_timer->stop();
    _touch_throttle.onDown(std::chrono::steady_clock::now());
    publishInput(CarPlayInput::Kind::TOUCH_DOWN, e->position());
}

void CarPlayWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (_touch_active)
    {
        publishTouchMove(e->position());
    }
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    _touch_active = false;
    _touch_flush_timer->stop();

    // The throttle decides whether the tail of the gesture still owes a move
    // before the release; see TouchThrottle::onUp for why.
    const auto flush =
        _touch_throttle.onUp(toThrottlePoint(e->position()), std::chrono::steady_clock::now());
    if (flush.has_value())
    {
        publishInput(CarPlayInput::Kind::TOUCH_MOVE, QPointF(flush->x, flush->y));
    }
    publishInput(CarPlayInput::Kind::TOUCH_UP, e->position());
}
