#ifndef CARPLAY_WIDGET_H_
#define CARPLAY_WIDGET_H_

#include "carplay/touch_action.h"
#include "carplay/message.h"
#include "carplay/config.h"
#include "carplay/device_step.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMouseEvent>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <string_view>

// Forward declarations for libavcodec stuff.
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Forward declarations for libusb to keep things tidy.
struct libusb_device_handle;


class CarPlayWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    using config_t = CarplayConfig_t;
    static constexpr std::string_view kWidgetName = "carplay";

    CarPlayWidget(CarplayConfig_t cfg);
    ~CarPlayWidget();

    void setSize(uint32_t width_px, uint32_t height_px);
    
    // Integrated decoding interface (now used internally)
    void accept_new_data(const uint8_t* buffer, uint32_t buffer_len);
    void stop_decoder();
    
    // Dongle driver functionality
    void start_dongle();
    void stop_dongle();
    void step(); // Public so callbacks can access it
    void register_audio_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb);
    void register_phone_connect_event(std::function<void(bool)> cb);

  signals:
    void touchEvent(TouchAction action, uint32_t x, uint32_t y);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

  private:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

    void setupShaders();
    void setupTextures();
    void uploadYUVTextures(const AVFrame* frame);
    
    // Integrated decoding methods
    void initializeDecoder();
    void cleanupDecoder();
    void run_decode_thread();
    void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt);
    void notifyFrameReady(); // Direct notification instead of timer

    // Dongle driver methods
    void initializeDongleDriver();
    void cleanupDongleDriver();
    bool find_dongle();
    void dongle_read_thread();
    void libusb_event_thread();
    void heartbeat_thread();
    void decode_dongle_response(MessageHeader header, const uint8_t* buffer);
    void send_touch_event_internal(TouchAction action, uint32_t x, uint32_t y);

    // Simplified OpenGL rendering members
    QOpenGLShaderProgram* m_shaderProgram;
    GLuint m_textureY;
    GLuint m_textureU;
    GLuint m_textureV;
    GLuint m_vbo;
    
    bool m_hasFrame;
    
    // Integrated decode thread members
    const AVCodec* _codec;
    AVCodecParserContext* _parser;
    AVCodecContext* _codec_context;
    AVFrame* _frame;
    AVPacket* _pkt;

    uint8_t _receive_buffer[512u * 1024u];
    uint32_t _receive_length;

    std::mutex _decode_mutex;
    std::condition_variable _decode_cv;
    std::atomic<bool> _should_terminate;
    std::thread _decode_thread;
    
    // Simplified frame buffer (single frame, direct notification)
    std::mutex _frame_mutex;
    AVFrame* _current_frame;
    std::atomic<bool> _new_frame_available;
    
    // Dongle driver constants
    static constexpr uint16_t kUsbVid = 0x1314u;
    static constexpr uint16_t kUsbPid = 0x1521u;
    static constexpr uint8_t kInterfaceNumber = 0x00;
    static constexpr uint8_t kEndpointInAddress = 0x81u;
    static constexpr uint8_t kEndpointOutAddress = 0x01u;
    static constexpr uint16_t kUsbDefaultTimeoutMs = 1000u;
    static constexpr uint16_t kUsbConfigDelayMs = 25u;
    static constexpr uint16_t kHeartbeatTimeMs = 2000u;
    
    // Dongle driver member variables
    CarplayConfig_t _cfg;

    libusb_device_handle* _device_handle;
    int _hotplug_callback_handle;
    DeviceStep _current_step;

    std::thread _event_thread;
    std::thread _read_thread;
    std::thread _heartbeat_thread;
    std::atomic<bool> _event_thread_should_run;
    std::atomic<bool> _read_thread_should_run;
    std::atomic<bool> _heartbeat_thread_should_run;

    std::vector<uint8_t> _usb_request;
    uint8_t _rx_data[512u * 1024u];

    std::function<void(const uint8_t* buffer, uint32_t buffer_len)> _audio_ready_callback;
    std::function<void(bool)> _phone_connect_event_callback;
    
    // Thread safety
    std::mutex _step_mutex;
};

#endif  // CARPLAY_WIDGET_H_