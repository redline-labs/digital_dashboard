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
#include "carplay/touch_throttle.h"

#include <QtCore/QObject>
#include <atomic>
#include <chrono>
#include <deque>
#include <vector>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Forward declarations for libavcodec/libswscale.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class QAudioSink;
class QAudioSource;
class QIODevice;
class QTimer;

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
    void resizeEvent(QResizeEvent* event) override;
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
    void decodeAccessUnit(const uint8_t* annexb, size_t len);
    // Converts and scales the decoded frame straight into the back buffer, then
    // publishes it as the new front buffer. Returns false if the frame was
    // unusable.
    bool renderFrameToBackBuffer(const AVFrame* frame);

    // Publishes immediately, with no rate limiting. Everything that reaches the
    // phone goes through here, so it is also what stamps _last_touch_sent.
    void publishInput(CarPlayInput::Kind kind, const QPointF& pos);
    // Rate-limited entry point for drag motion. Down and up are state
    // transitions and always publish immediately; only motion is limited.
    void publishTouchMove(const QPointF& pos);
    static TouchThrottle::Point toThrottlePoint(const QPointF& p) { return {p.x(), p.y()}; }
    // Publishes the current widget size to _target_size for the decode thread.
    void publishTargetSize();

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
    // Reusable Annex-B assembly buffer handed to _pkt, so neither the packet
    // nor the config+frame concatenation allocates per frame. Always carries
    // AV_INPUT_BUFFER_PADDING_SIZE zeroed trailing bytes for the decoder.
    std::vector<uint8_t> _au_buf;
    // YUV420 -> BGRA scaler, plus the inputs it was built for so it is rebuilt
    // only when the frame geometry, pixel format or colour range changes.
    // It also does the scale to widget size, so the conversion and the resize
    // are a single pass and paintEvent never has to transform anything.
    SwsContext* _sws = nullptr;
    int _sws_width = 0;
    int _sws_height = 0;
    int _sws_dst_width = 0;
    int _sws_dst_height = 0;
    int _sws_src_format = -1;  // AVPixelFormat
    bool _sws_full_range = false;

    // Widget size published by the GUI thread for the decode thread to scale
    // to, packed as (width << 32 | height) so the pair is read atomically --
    // two separate atomics could tear and yield a mismatched size. Zero until
    // the first resize, which means "use the frame's own size".
    std::atomic<uint64_t> _target_size{0};

    // Decoded frames, shared between subscriber and GUI threads. Ping-pong
    // buffers: the subscriber thread converts into _frames[_back] with no
    // allocation, then takes the lock only to publish the index. paintEvent
    // holds the lock for the duration of its blit, which is what stops the
    // decoder from swapping a buffer out from under a live draw.
    std::mutex _frame_mutex;
    QImage _frames[2];
    int _front_frame = -1;  // index of the paintable frame, -1 until the first
    int _back_frame = 0;

    // Session state for the placeholder overlay, guarded by _frame_mutex.
    std::string _status_text = "Waiting for CarPlay driver";

    // GUI-thread only.
    std::unique_ptr<pub_sub::ZenohPublisher<CarPlayInput>> _input_pub;
    bool _touch_active = false;

    // Touch rate limiting. Every mouse event and the flush timer run on the GUI
    // thread, so none of this needs synchronising.
    //
    // _touch_throttle holds the policy and the deferred position; this widget
    // only supplies the clock, the timer and the publishing. The timer is what
    // makes the trailing flush happen -- the part that is easy to leave out and
    // wrong to, since a drag that stops moving but keeps the button down
    // produces no further events, and without a flush the last position is
    // thrown away and the phone's idea of the finger stays an interval behind.
    QTimer* _touch_flush_timer = nullptr;  // single-shot, owned by Qt
    TouchThrottle _touch_throttle;

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
