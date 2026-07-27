#include "carplay/carplay_widget.h"

#include <spdlog/spdlog.h>
#include <QPainter>
#include <QtMultimedia/QAbstractVideoBuffer>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QAudioSource>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoFrameFormat>
#include <QtMultimedia/QVideoSink>
#include <QtMultimediaWidgets/QVideoWidget>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstring>

namespace
{
// At most this many decoded frames may be in flight toward the GUI thread.
// Two covers one being presented plus one queued; beyond that the GUI thread
// is behind and the newest frame is worth more than a backlog of stale ones.
constexpr int kMaxSinkFramesInFlight = 2;

// Lets a QVideoFrame reference libavcodec's decoded planes directly instead of
// copying them into a Qt-owned buffer. The wrapped AVFrame is a ref (not a
// deep copy) of the decoder's frame, so it keeps that buffer alive for exactly
// as long as Qt holds the QVideoFrame.
class AVFrameVideoBuffer : public QAbstractVideoBuffer
{
  public:
    AVFrameVideoBuffer(AVFrame* owned_ref, QVideoFrameFormat format) :
        _frame(owned_ref),
        _format(std::move(format))
    {
    }

    ~AVFrameVideoBuffer() override { av_frame_free(&_frame); }

    AVFrameVideoBuffer(const AVFrameVideoBuffer&) = delete;
    AVFrameVideoBuffer& operator=(const AVFrameVideoBuffer&) = delete;

    MapData map(QVideoFrame::MapMode /*mode*/) override
    {
        // Planar 4:2:0: the chroma planes are half height, rounded up.
        const int chroma_height = (_frame->height + 1) / 2;
        MapData data;
        data.planeCount = 3;
        for (int plane = 0; plane < 3; ++plane)
        {
            data.data[plane] = _frame->data[plane];
            data.bytesPerLine[plane] = _frame->linesize[plane];
            data.dataSize[plane] =
                _frame->linesize[plane] * ((plane == 0) ? _frame->height : chroma_height);
        }
        return data;
    }

    QVideoFrameFormat format() const override { return _format; }

  private:
    AVFrame* _frame;
    QVideoFrameFormat _format;
};
}  // namespace

CarPlayWidget::CarPlayWidget(CarplayConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
    setAttribute(Qt::WA_OpaquePaintEvent);

    // Decoded YUV planes go straight to Qt Multimedia, which converts them to
    // RGB in a shader on the GPU (Metal on macOS, Vulkan/OpenGL on Linux).
    //
    // NOTE: this child surface composites ON TOP of any sibling widget that
    // overlaps it, even one explicitly raise()d above it -- measured, and not
    // detectable via WA_NativeWindow, which reports false. Anything that needs
    // to draw over the CarPlay video has to be a child of _video_widget, not a
    // sibling of this widget. See docs/carplay_bringup.md stage 8.
    _video_widget = new QVideoWidget(this);
    _video_widget->setGeometry(rect());
    // Touch is this widget's job. Without this the video child is the hit-test
    // target, and a press only reaches us by propagation -- which also leaves
    // the mouse grab on the child, breaking drags.
    _video_widget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Stays hidden until the first frame so the status text shows through.
    _video_widget->hide();

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

    auto data = reader.getData();

    // Parameter sets on their own are not a decodable access unit -- feeding
    // them straight to libavcodec yields AVERROR_INVALIDDATA. Cache them and
    // prepend to the next access unit instead (Annex-B concatenates freely).
    if (reader.getIsConfig())
    {
        _pending_config.assign(data.begin(), data.end());
        return;
    }

    // Assemble [pending parameter sets][access unit] into the reusable buffer.
    // libavcodec reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end of
    // a packet it does not own, so the tail must exist and be zeroed.
    const size_t len = _pending_config.size() + data.size();
    _au_buf.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(_au_buf.data(), _pending_config.data(), _pending_config.size());
    std::memcpy(_au_buf.data() + _pending_config.size(), data.begin(), data.size());
    std::memset(_au_buf.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    _pending_config.clear();

    decodeAccessUnit(_au_buf.data(), len);
}

void CarPlayWidget::decodeAccessUnit(const uint8_t* data, size_t len)
{
    if (len == 0)
    {
        return;
    }

    // The driver publishes whole Annex-B access units, so no parser is needed.
    // The packet borrows the caller's padded buffer rather than allocating and
    // copying: send_packet does not take ownership, and libavcodec makes its own
    // reference if it needs the bytes beyond this call.
    _pkt->data = const_cast<uint8_t*>(data);
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

    while (avcodec_receive_frame(_codec_context, _frame) == 0)
    {
        if (!presentFrameToVideoSink(_frame))
        {
            // Anything the sink cannot accept lands here and would render black.
            if (++_convert_errors % 60 == 1)
            {
                SPDLOG_WARN("[carplay] cannot present decoded frame ({}x{}, pix_fmt {}); "
                            "{} frame(s) dropped",
                            _frame->width, _frame->height, _frame->format, _convert_errors);
            }
            continue;
        }

        if (!_rendered_first_frame)
        {
            _rendered_first_frame = true;
            SPDLOG_INFO("[carplay] first video frame decoded and presented ({}x{})",
                        _frame->width, _frame->height);
        }
    }
}

bool CarPlayWidget::presentFrameToVideoSink(const AVFrame* frame)
{
    if (frame == nullptr || frame->data[0] == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return false;
    }

    // Only planar 4:2:0 is mapped here -- it is what the software H.264/H.265
    // decoders produce. Anything else falls back to the caller's error path.
    const bool is_420p =
        (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P);
    if (!is_420p)
    {
        return false;
    }

    // Drop rather than queue without bound: each in-flight frame pins a buffer
    // from the decoder's pool.
    if (_sink_frames_in_flight.load(std::memory_order_acquire) >= kMaxSinkFramesInFlight)
    {
        return true;  // not an error -- deliberately shedding a late frame
    }

    QVideoFrameFormat format(QSize(frame->width, frame->height),
                             QVideoFrameFormat::Format_YUV420P);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange((frame->format == AV_PIX_FMT_YUVJ420P ||
                          frame->color_range == AVCOL_RANGE_JPEG)
                             ? QVideoFrameFormat::ColorRange_Full
                             : QVideoFrameFormat::ColorRange_Video);

    // A ref, not a copy: shares the decoder's buffer and holds it alive.
    AVFrame* frame_ref = av_frame_clone(frame);
    if (frame_ref == nullptr)
    {
        return false;
    }

    QVideoFrame video_frame(std::make_unique<AVFrameVideoBuffer>(frame_ref, std::move(format)));

    // QVideoSink is not thread-safe, so presentation hops to the GUI thread.
    // QVideoFrame is implicitly shared, so this hands over the reference
    // without copying pixels.
    _sink_frames_in_flight.fetch_add(1, std::memory_order_release);
    QMetaObject::invokeMethod(
        this,
        [this, video_frame]() mutable {
            if (_video_widget != nullptr)
            {
                if (!_video_widget->isVisible())
                {
                    _video_widget->show();
                }
                _video_widget->videoSink()->setVideoFrame(video_frame);
            }
            // Release only after the frame has been handed to the sink.
            video_frame = QVideoFrame();
            _sink_frames_in_flight.fetch_sub(1, std::memory_order_release);
        },
        Qt::QueuedConnection);
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
    // Video pixels never reach the CPU -- _video_widget owns the frame surface
    // and covers this widget entirely once frames are flowing. All that is left
    // here is the placeholder shown before the first frame arrives.
    if (_video_widget != nullptr && _video_widget->isVisible())
    {
        return;
    }

    std::string status;
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        status = _status_text;
    }

    QPainter p(this);
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
    if (_video_widget != nullptr)
    {
        _video_widget->setGeometry(rect());
    }
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

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    _touch_active = true;
    publishInput(CarPlayInput::Kind::TOUCH_DOWN, e->position());
}

void CarPlayWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (_touch_active)
    {
        publishInput(CarPlayInput::Kind::TOUCH_MOVE, e->position());
    }
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    _touch_active = false;
    publishInput(CarPlayInput::Kind::TOUCH_UP, e->position());
}
