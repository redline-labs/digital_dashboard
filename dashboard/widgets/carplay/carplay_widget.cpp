#include "carplay/carplay_widget.h"

#include <spdlog/spdlog.h>
#include <QPainter>
#include <QTimer>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QAudioSource>
#include <QtMultimedia/QMediaDevices>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
}

CarPlayWidget::CarPlayWidget(CarplayConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(std::move(cfg))
{
    setAttribute(Qt::WA_OpaquePaintEvent);

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
    if (_pkt != nullptr) av_packet_free(&_pkt);
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

    if (!_pending_config.empty())
    {
        std::vector<uint8_t> combined;
        combined.reserve(_pending_config.size() + data.size());
        combined.insert(combined.end(), _pending_config.begin(), _pending_config.end());
        combined.insert(combined.end(), data.begin(), data.end());
        _pending_config.clear();
        decodeAccessUnit(combined.data(), combined.size());
        return;
    }

    decodeAccessUnit(data.begin(), data.size());
}

void CarPlayWidget::decodeAccessUnit(const uint8_t* data, size_t len)
{
    if (len == 0)
    {
        return;
    }

    // The driver publishes whole Annex-B access units, so no parser is needed.
    if (av_new_packet(_pkt, static_cast<int>(len)) < 0)
    {
        return;
    }
    std::copy_n(data, len, _pkt->data);

    int ret = avcodec_send_packet(_codec_context, _pkt);
    av_packet_unref(_pkt);
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
        QImage img = convertYuv420ToRgbImage(_frame);
        if (img.isNull())
        {
            // Anything other than YUV420P lands here and would render black.
            if (++_convert_errors % 60 == 1)
            {
                SPDLOG_WARN("[carplay] cannot convert decoded frame to RGB ({}x{}, pix_fmt {}); "
                            "{} frame(s) dropped",
                            _frame->width, _frame->height, _frame->format, _convert_errors);
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(_frame_mutex);
            _current_image = std::move(img);
            frame_updated = true;
        }
        if (!_rendered_first_frame)
        {
            _rendered_first_frame = true;
            SPDLOG_INFO("[carplay] first video frame decoded and rendered ({}x{})",
                        _frame->width, _frame->height);
        }
    }

    if (frame_updated)
    {
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }
}

QImage CarPlayWidget::convertYuv420ToRgbImage(const AVFrame* frame)
{
    if (!frame || !frame->data[0] || frame->format != AV_PIX_FMT_YUV420P) return {};
    const int w = frame->width;
    const int h = frame->height;
    QImage img(w, h, QImage::Format_RGB888);
    if (img.isNull()) return {};

    const uint8_t* yPlane = frame->data[0];
    const uint8_t* uPlane = frame->data[1];
    const uint8_t* vPlane = frame->data[2];
    const int yStride = frame->linesize[0];
    const int uStride = frame->linesize[1];
    const int vStride = frame->linesize[2];

    for (int j = 0; j < h; ++j) {
        uint8_t* dst = img.scanLine(j);
        const uint8_t* yRow = yPlane + j * yStride;
        const uint8_t* uRow = uPlane + (j / 2) * uStride;
        const uint8_t* vRow = vPlane + (j / 2) * vStride;
        for (int i = 0; i < w; ++i) {
            int Y = yRow[i];
            int U = uRow[i / 2] - 128;
            int V = vRow[i / 2] - 128;
            int R = Y + ((91881 * V) >> 16);
            int G = Y - ((22554 * U + 46802 * V) >> 16);
            int B = Y + ((116130 * U) >> 16);
            dst[3*i + 0] = static_cast<uint8_t>(std::clamp(B, 0, 255));
            dst[3*i + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
            dst[3*i + 2] = static_cast<uint8_t>(std::clamp(R, 0, 255));
        }
    }
    return img;
}

void CarPlayWidget::onAudioMessage(CarPlayAudio::Reader reader)
{
    auto pcm = reader.getPcm();
    if (pcm.size() == 0)
    {
        return;
    }

    // Hop to the GUI thread: QAudioSink and its QIODevice are not thread-safe
    // and must be used from the thread that owns them.
    QByteArray bytes(reinterpret_cast<const char*>(pcm.begin()), static_cast<int>(pcm.size()));
    const int rate = static_cast<int>(reader.getSampleRateHz());
    const int channels = reader.getChannels();
    QMetaObject::invokeMethod(
        this, [this, bytes = std::move(bytes), rate, channels] { playAudioChunk(bytes, rate, channels); },
        Qt::QueuedConnection);
}

void CarPlayWidget::ensureAudioSink(int sample_rate, int channels)
{
    if (_audio_sink != nullptr && sample_rate == _sink_sample_rate && channels == _sink_channels)
    {
        return;
    }

    QAudioFormat format;
    format.setSampleRate(sample_rate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(format))
    {
        SPDLOG_WARN("[carplay] audio format {} Hz / {} ch unsupported by '{}'; using its preferred format",
                    sample_rate, channels, device.description().toStdString());
        format = device.preferredFormat();
    }

    _audio_sink = std::make_unique<QAudioSink>(device, format);
    _audio_device = _audio_sink->start();
    _sink_sample_rate = sample_rate;
    _sink_channels = channels;

    if (_audio_device == nullptr)
    {
        SPDLOG_ERROR("[carplay] failed to start audio sink ({} Hz / {} ch)", sample_rate, channels);
        _audio_sink.reset();
        _sink_sample_rate = 0;
        _sink_channels = 0;
        return;
    }
    SPDLOG_INFO("[carplay] audio sink started: {} Hz / {} ch on '{}'",
                format.sampleRate(), format.channelCount(), device.description().toStdString());
}

void CarPlayWidget::playAudioChunk(const QByteArray& pcm, int sample_rate, int channels)
{
    if (sample_rate <= 0 || channels <= 0)
    {
        return;
    }
    ensureAudioSink(sample_rate, channels);
    if (_audio_device == nullptr)
    {
        return;
    }

    const qint64 written = _audio_device->write(pcm.constData(), pcm.size());
    if (written < pcm.size())
    {
        // The driver paces the stream; a short write means the sink buffer is
        // full, which shows up as a dropout. Worth seeing during bring-up.
        SPDLOG_DEBUG("[carplay] audio sink short write: {} of {} bytes", written, pcm.size());
    }
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

    QImage img;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        img = _current_image;
        status = _status_text;
    }

    if (!img.isNull())
    {
        p.drawImage(rect(), img);
        return;
    }

    p.fillRect(rect(), Qt::black);
    if (!status.empty())
    {
        p.setPen(QColor(0x88, 0x88, 0x88));
        p.drawText(rect(), Qt::AlignCenter, QString::fromStdString(status));
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
