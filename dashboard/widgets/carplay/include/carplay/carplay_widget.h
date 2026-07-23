#ifndef CARPLAY_WIDGET_H_
#define CARPLAY_WIDGET_H_

#include "carplay/config.h"
#include "dashboard/widget_types.h"

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "carplay_video.capnp.h"
#include "carplay_audio.capnp.h"
#include "carplay_input.capnp.h"
#include "carplay_session.capnp.h"

#include <QtWidgets/QWidget>
#include <QtGui/QMouseEvent>
#include <QImage>
#include <QtMultimedia/QAudioFormat>

#include "carplay/audio_ring.h"

#include <QtCore/QObject>
#include <atomic>
#include <deque>
#include <vector>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Forward declarations for libavcodec.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class QAudioSink;
class QAudioSource;
class QIODevice;

// Thin client of the carplay driver node: renders the H.264/H.265 video
// stream published on zenoh and forwards touch input back to the driver.
// All phone/USB/AirPlay interaction lives in nodes/carplay.
class CarPlayWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = CarplayConfig_t;
    static constexpr std::string_view kFriendlyName = "CarPlay";
    static constexpr widget_type_t kWidgetType = widget_type_t::carplay;

    CarPlayWidget(CarplayConfig_t cfg, QWidget* parent = nullptr);
    ~CarPlayWidget();
    const config_t& getConfig() const { return _cfg; }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

  private slots:
    void pumpMicrophone();

  private:
    // Runs on the zenoh subscriber thread.
    void onVideoMessage(CarPlayVideo::Reader reader);
    void onAudioMessage(CarPlayAudio::Reader reader);
    void onSessionMessage(CarPlaySessionState::Reader reader);

    // Recreates the sink and ring buffer when the phone changes format. Safe to
    // call from the subscriber thread; hops to the GUI thread internally.
    void ensureAudioSink(int sample_rate, int channels);
    void startMicrophone(int sample_rate, int channels);
    void stopMicrophone();

    bool ensureDecoder(CarPlayVideo::Codec codec);
    void destroyDecoder();
    void decodeAccessUnit(const uint8_t* data, size_t len);
    QImage convertYuv420ToRgbImage(const AVFrame* frame);

    void publishInput(CarPlayInput::Kind kind, const QPointF& pos);

    CarplayConfig_t _cfg;

    // Video decode state; only touched from the video subscriber thread.
    AVCodecContext* _codec_context = nullptr;
    AVFrame* _frame = nullptr;
    AVPacket* _pkt = nullptr;
    CarPlayVideo::Codec _codec_id = CarPlayVideo::Codec::H264;
    // Set once we've seen a sync point (parameter sets or a keyframe); frames
    // before that would only produce decoder errors.
    bool _synced = false;
    uint32_t _dropped_before_sync = 0;
    uint32_t _decode_errors = 0;
    uint32_t _convert_errors = 0;
    bool _rendered_first_frame = false;
    uint32_t _last_seq = 0;
    // Parameter sets awaiting the next access unit to be prepended to.
    std::vector<uint8_t> _pending_config;

    // Latest decoded frame, shared between subscriber and GUI threads.
    std::mutex _frame_mutex;
    QImage _current_image;

    // Session state for the placeholder overlay, guarded by _frame_mutex.
    std::string _status_text = "Waiting for CarPlay driver";

    // GUI-thread only.
    std::unique_ptr<pub_sub::ZenohPublisher<CarPlayInput>> _input_pub;
    bool _touch_active = false;

    // Audio playback. The sink runs in pull mode: it drains _audio_ring on its
    // own audio thread, decoupled from the bursty network delivery. The zenoh
    // subscriber thread pushes PCM into the ring directly (it is thread-safe),
    // so audio no longer competes with video on the GUI event loop.
    std::unique_ptr<QAudioSink> _audio_sink;
    std::unique_ptr<AudioRingBuffer> _audio_ring;
    std::atomic<int> _sink_sample_rate{0};
    std::atomic<int> _sink_channels{0};

    // Microphone capture for Siri/calls, published back to the driver.
    std::unique_ptr<QAudioSource> _mic_source;
    QIODevice* _mic_device = nullptr;  // owned by _mic_source
    std::unique_ptr<pub_sub::ZenohPublisher<CarPlayAudio>> _mic_pub;
    int _mic_sample_rate = 0;
    int _mic_channels = 0;

    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayVideo>> _video_sub;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayAudio>> _audio_sub;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlaySessionState>> _session_sub;
};

#endif  // CARPLAY_WIDGET_H_
