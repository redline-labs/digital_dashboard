#include "carplay/carplay_widget.h"
#include <QtGui/QImage>
#include "carplay/touch_action.h"
#include "carplay/dongle_config_file.h"
#include "carplay/audio_type.h"
#include "carplay/command_mapping.h"
#include "carplay/libusb_helpers.h"

#include <spdlog/spdlog.h>
#include <QtGui/QWindow>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QMediaDevices>
#include <vector>
#include <chrono>
#include <libusb.h>
#include <QPainter>

extern "C" {
#include <libavcodec/avcodec.h>
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

// moved AV includes above

// Libusb helper functions
static void libusb_log(libusb_context* /*ctx*/, enum libusb_log_level level, const char* str)
{
    switch (level)
    {
        case LIBUSB_LOG_LEVEL_ERROR:
            SPDLOG_ERROR(str);
            break;
        case LIBUSB_LOG_LEVEL_WARNING:
            SPDLOG_WARN(str);
            break;
        case LIBUSB_LOG_LEVEL_INFO:
            SPDLOG_INFO(str);
            break;
        case LIBUSB_LOG_LEVEL_DEBUG:
        default:
            SPDLOG_DEBUG(str);
            break;
    }
}

static int hotplug_callback(libusb_context* /*ctx*/, libusb_device* /*device*/, libusb_hotplug_event event, void* widget_instance)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
    {
        static_cast<CarPlayWidget*>(widget_instance)->step();
    }
    else
    {
        SPDLOG_INFO("Dongle was removed!");
        static_cast<CarPlayWidget*>(widget_instance)->stop_dongle();
    }
    return 0;
}

static void libusb_transfer_callback(struct libusb_transfer *transfer)
{
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        static_cast<CarPlayWidget*>(transfer->user_data)->step();
    }
    else
    {
        SPDLOG_ERROR("{} : transferred {} bytes (expected {}).",
            lookup_libusb_transfer_status_string(transfer->status),
            transfer->actual_length,
            transfer->length);
    }
    libusb_free_transfer(transfer);
}

static uint32_t read_uint32_t_little_endian(const uint8_t* buffer)
{
    return
        (static_cast<uint32_t>(buffer[0]) <<  0) |
        (static_cast<uint32_t>(buffer[1]) <<  8) |
        (static_cast<uint32_t>(buffer[2]) << 16) |
        (static_cast<uint32_t>(buffer[3]) << 24);
}

CarPlayWidget::CarPlayWidget(CarplayConfig_t cfg, QWidget* parent) :
    QWidget(parent),
    m_hasFrame(false),
    _codec(nullptr),
    _parser(nullptr),
    _codec_context(nullptr),
    _frame(nullptr),
    _pkt(nullptr),
    _receive_length(0),
    _should_terminate(false),
    _current_frame(nullptr),
    _new_frame_available(false),
    _cfg{cfg},
    _device_handle{nullptr},
    _current_step{DeviceStep::Init},
    _event_thread_should_run{true},
    _read_thread_should_run{true},
    _heartbeat_thread_should_run{true},
    _audio_ready_callback{nullptr},
    _phone_connect_event_callback{nullptr}
{
    setStyleSheet("QWidget { background-color : black; }");
    initializeDecoder();
    initializeDongleDriver();


    QAudioFormat format;
    format.setSampleRate(44100);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);

    const auto devices = QMediaDevices::audioOutputs();
    for (const QAudioDevice &device : devices)
    {
        SPDLOG_DEBUG("Found audio device: {}",  device.description().toStdString());
    }

    QAudioDevice info(QMediaDevices::defaultAudioOutput());
    if (!info.isFormatSupported(format)) {
        SPDLOG_ERROR("Raw audio format not supported by backend, cannot play audio.");
        // TODO: Should we bomb out?
    }

    SPDLOG_INFO("Using audio output: {}", info.description().toStdString());

    QAudioSink audio(format);
    SPDLOG_DEBUG("Default audio sink buffer size = {}, setting to {}.", audio.bufferSize(), cfg.audio_device_buffer_size);
    audio.setBufferSize(cfg.audio_device_buffer_size);

    auto audio_buffer = audio.start();

    // Register audio callback with the integrated CarPlay widget
    register_audio_ready_callback([&audio_buffer] (const uint8_t* buffer, uint32_t buffer_len){
        audio_buffer->write(reinterpret_cast<const char*>(buffer), buffer_len);
    });

    // Start the integrated dongle functionality
    start_dongle();

    SPDLOG_INFO("CarPlay widget configured and started.");
}

CarPlayWidget::~CarPlayWidget()
{
    stop_decoder();
    stop_dongle();
    cleanupDecoder();
    cleanupDongleDriver();
    
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        if (_current_frame) {
            av_frame_unref(_current_frame);
            av_frame_free(&_current_frame);
        }
    }
}

void CarPlayWidget::setSize(uint32_t width_px, uint32_t height_px)
{
    setFixedSize({static_cast<int>(width_px), static_cast<int>(height_px)});
}

// OpenGL initialization removed; rendering handled in paintEvent

// OpenGL shaders removed

// OpenGL textures removed

void CarPlayWidget::initializeDecoder()
{
    // Initialize codec components
    _pkt = av_packet_alloc();
    _codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    _parser = av_parser_init(_codec->id);
    _codec_context = avcodec_alloc_context3(_codec);
    _frame = av_frame_alloc();

    if (!_pkt || !_codec || !_parser || !_codec_context || !_frame) {
        SPDLOG_ERROR("Failed to initialize codec components");
        return;
    }

    // Use software decoding
    if (avcodec_open2(_codec_context, _codec, nullptr) >= 0) {
        SPDLOG_INFO("Software decoding enabled");
    } else {
        SPDLOG_ERROR("Failed to open codec");
        return;
    }

    _decode_thread = std::thread(&CarPlayWidget::run_decode_thread, this);
}

void CarPlayWidget::cleanupDecoder()
{
    if (_parser) av_parser_close(_parser);
    if (_codec_context) avcodec_free_context(&_codec_context);
    if (_frame) av_frame_free(&_frame);
    if (_pkt) av_packet_free(&_pkt);
}

void CarPlayWidget::accept_new_data(const uint8_t* buffer, uint32_t buffer_len)
{
    {
        std::lock_guard lk(_decode_mutex);
        std::copy_n(buffer, std::min(buffer_len, static_cast<uint32_t>(sizeof(_receive_buffer))), _receive_buffer);
        _receive_length = buffer_len;
    }
    _decode_cv.notify_one();
}

void CarPlayWidget::stop_decoder()
{
    _should_terminate = true;
    _decode_cv.notify_all();
    if (_decode_thread.joinable()) {
        _decode_thread.join();
    }
}

void CarPlayWidget::run_decode_thread()
{
    while (!_should_terminate) {
        auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        
        std::unique_lock lk(_decode_mutex);
        auto res = _decode_cv.wait_until(lk, timeout);
        if (res == std::cv_status::timeout) continue;

        const uint8_t* data = _receive_buffer;
        uint32_t data_len = _receive_length;

        while (data_len > 0) {
            int ret = av_parser_parse2(_parser, _codec_context, &_pkt->data, &_pkt->size,
                                      data, data_len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) break;
            
            data += ret;
            data_len -= ret;

            if (_pkt->size) {
                decode(_codec_context, _frame, _pkt);
            }
        }
    }
}

void CarPlayWidget::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    if (avcodec_send_packet(dec_ctx, pkt) < 0) return;

    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        // Basic validation
        if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096) {
            continue;
        }

        // Validate frame format and data
        if ((frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) ||
            !frame->data[0] || !frame->data[1] || !frame->data[2]) {
            continue;
        }

        // Update frame buffer
        {
            std::lock_guard<std::mutex> lock(_frame_mutex);
            
            // Clean up previous frame
            if (_current_frame) {
                av_frame_unref(_current_frame);
            } else {
                _current_frame = av_frame_alloc();
            }
            
            // Create a reference to the current frame
            if (_current_frame && av_frame_ref(_current_frame, frame) >= 0) {
                _new_frame_available = true;
                m_hasFrame = true;
                notifyFrameReady();
            }
        }
    }
}

void CarPlayWidget::notifyFrameReady()
{
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void CarPlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    if (!m_hasFrame) return;
    if (_new_frame_available.load()) {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        if (_current_frame) {
            QImage img = convertYuv420ToRgbImage(_current_frame);
            _new_frame_available = false;
            if (!img.isNull()) {
                QPainter p(this);
                p.drawImage(rect(), img);
            }
        }
    }
}

// no-op resize; QWidget handles repaint

// OpenGL texture upload removed; handled by MetalWindow

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    uint32_t x = static_cast<uint32_t>(e->pos().x());
    uint32_t y = static_cast<uint32_t>(e->pos().y());
    emit touchEvent(TouchAction::Down, x, y);
    send_touch_event_internal(TouchAction::Down, x, y);
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    uint32_t x = static_cast<uint32_t>(e->pos().x());
    uint32_t y = static_cast<uint32_t>(e->pos().y());
    emit touchEvent(TouchAction::Up, x, y);
    send_touch_event_internal(TouchAction::Up, x, y);
}

void CarPlayWidget::mouseMoveEvent(QMouseEvent* e)
{
    uint32_t x = static_cast<uint32_t>(e->pos().x());
    uint32_t y = static_cast<uint32_t>(e->pos().y());
    emit touchEvent(TouchAction::Move, x, y);
    send_touch_event_internal(TouchAction::Move, x, y);
}

// Dongle driver implementation
void CarPlayWidget::initializeDongleDriver()
{
    const auto version = libusb_get_version();
    SPDLOG_DEBUG("Using libusb {}.{}.{}", version->major, version->minor, version->micro);
    
    libusb_init_context(nullptr, nullptr, /*num_options=*/0);
    
    if (_cfg.libusb_debug == true)
    {
        libusb_set_log_cb(nullptr, &libusb_log, LIBUSB_LOG_CB_GLOBAL);
        libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }
    
    _event_thread = std::thread(&CarPlayWidget::libusb_event_thread, this);
    
    // Register hotplug callback.
    libusb_hotplug_register_callback(
        nullptr,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE,
        kUsbVid,
        kUsbPid,
        LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_callback,
        this,
        &_hotplug_callback_handle
    );
}

void CarPlayWidget::cleanupDongleDriver()
{
    libusb_hotplug_deregister_callback(nullptr, _hotplug_callback_handle);
    
    _event_thread_should_run = false;
    if (_event_thread.joinable())
    {
        _event_thread.join();
    }
    
    if (_device_handle != nullptr)
    {
        libusb_release_interface(_device_handle, kInterfaceNumber);
        libusb_close(_device_handle);
    }
    
    libusb_exit(nullptr);
}

void CarPlayWidget::start_dongle()
{
    step();
}

void CarPlayWidget::stop_dongle()
{
    _current_step = DeviceStep::Init;
    
    // Stop the read thread.
    _read_thread_should_run = false;
    if (_read_thread.joinable())
    {
        _read_thread.join();
    }
    
    // Stop the heartbeat thread.
    _heartbeat_thread_should_run = false;
    if (_heartbeat_thread.joinable())
    {
        _heartbeat_thread.join();
    }
    
    if (_device_handle != nullptr)
    {
        SPDLOG_DEBUG("Cleaning up device handle.");
        libusb_release_interface(_device_handle, kInterfaceNumber);
        libusb_close(_device_handle);
    }
    
    _device_handle = nullptr;
}

void CarPlayWidget::register_audio_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb)
{
    _audio_ready_callback = cb;
}

void CarPlayWidget::register_phone_connect_event(std::function<void(bool)> cb)
{
    _phone_connect_event_callback = cb;
}

bool CarPlayWidget::find_dongle()
{
    _device_handle = libusb_open_device_with_vid_pid(nullptr, kUsbVid, kUsbPid);
    bool success = (_device_handle != nullptr);
    
    if (success)
    {
        int result = libusb_claim_interface(_device_handle, kInterfaceNumber);
        success = (result == 0);
    }
    
    return success;
}

void CarPlayWidget::step()
{
    std::lock_guard<std::mutex> lock(_step_mutex);
    
    try {
        switch (_current_step)
    {
        case (DeviceStep::Init):
            if (find_dongle() == false)
            {
                _current_step = DeviceStep::Fail;
                SPDLOG_ERROR("Failed to find dongle.");
                break;
            }
            
            // Kick off read thread.
            _read_thread_should_run = true;
            _read_thread = std::thread(&CarPlayWidget::dongle_read_thread, this);
            
            SPDLOG_DEBUG("Attached to dongle.");
            // Fall through;
            
        case (DeviceStep::SendDPI):
            SPDLOG_DEBUG("Sending config (DPI).");
            _usb_request = SendNumber(DongleConfigFile::DPI, _cfg.dpi).serialize();
            _current_step = DeviceStep::SendOpen;  // Next step on success;
            break;
            
        case (DeviceStep::SendOpen):
            SPDLOG_DEBUG("Sending config (Open).");
            _usb_request = SendOpen(_cfg).serialize();
            _current_step = DeviceStep::SendNightMode;  // Next step on success;
            break;
            
        case (DeviceStep::SendNightMode):
            SPDLOG_DEBUG("Sending config (Night Mode).");
            _usb_request = SendBoolean(DongleConfigFile::NightMode, _cfg.night_mode).serialize();
            _current_step = DeviceStep::SendDriveHand;  // Next step on success;
            break;
            
        case (DeviceStep::SendDriveHand):
            SPDLOG_DEBUG("Sending config (drive side).");
            _usb_request = SendNumber(DongleConfigFile::HandDriveMode, static_cast<uint32_t>(_cfg.drive_type)).serialize();
            _current_step = DeviceStep::SendChargeMode;  // Next step on success;
            break;
            
        case (DeviceStep::SendChargeMode):
            SPDLOG_DEBUG("Sending config (charge mode).");
            _usb_request = SendBoolean(DongleConfigFile::ChargeMode, true).serialize();
            _current_step = DeviceStep::SendBoxName;  // Next step on success;
            break;
            
        case (DeviceStep::SendBoxName):
            SPDLOG_DEBUG("Sending config (box name).");
            _usb_request = SendString(DongleConfigFile::BoxName, _cfg.box_name).serialize();
            _current_step = DeviceStep::SendBoxSettings;  // Next step on success;
            break;
            
        case (DeviceStep::SendBoxSettings):
            SPDLOG_DEBUG("Sending config (box settings).");
            _usb_request = SendBoxSettings(_cfg).serialize();
            _current_step = DeviceStep::SendWiFiEnable;  // Next step on success;
            break;
            
        case (DeviceStep::SendWiFiEnable):
            SPDLOG_DEBUG("Sending config (WiFi enable).");
            _usb_request = Command(CommandMapping::WifiEnable).serialize();
            _current_step = DeviceStep::SendWiFiType;  // Next step on success;
            break;
            
        case (DeviceStep::SendWiFiType):
            SPDLOG_DEBUG("Sending config (WiFi type).");
            _usb_request = Command(CommandMapping::Wifi5g).serialize();
            _current_step = DeviceStep::SendMicType;  // Next step on success;
            break;
            
        case (DeviceStep::SendMicType):
            SPDLOG_DEBUG("Sending config (Mic type).");
            _usb_request = Command(CommandMapping::Mic).serialize();
            _current_step = DeviceStep::SendAudioTransferMode;  // Next step on success;
            break;
            
        case (DeviceStep::SendAudioTransferMode):
            SPDLOG_DEBUG("Sending config (audio transfer mode).");
            _usb_request = Command(CommandMapping::AudioTransferOff).serialize();
            _current_step = DeviceStep::SendWiFiConnect;  // Next step on success;
            break;
            
        case (DeviceStep::SendWiFiConnect):
            _usb_request = Command(CommandMapping::WifiConnect).serialize();
            _current_step = DeviceStep::Done;  // Next step on success;
            break;
            
        case (DeviceStep::Done):
            _usb_request = {};
            if (!_heartbeat_thread.joinable())
            {
                _heartbeat_thread_should_run = true;
                _heartbeat_thread = std::thread(&CarPlayWidget::heartbeat_thread, this);
            }
            break;
            
        case (DeviceStep::Fail):
            SPDLOG_ERROR("FUCK!");
            _usb_request = {};
            break;
            
        default:
            SPDLOG_ERROR("Invalid initialization.");
            _usb_request = {};
            break;
    }
    
    if (_usb_request.empty() == false)
    {
        bool success = false;
        
        struct libusb_transfer* transfer = libusb_alloc_transfer(0u);
        success = transfer != nullptr;
        if (success == false)
        {
            SPDLOG_ERROR("Failed to allocate libusb transfer.");
        }
        
        if (success == true)
        {
            libusb_fill_bulk_transfer(
                transfer,
                _device_handle,
                kEndpointOutAddress,
                &_usb_request[0],
                _usb_request.size(),
                libusb_transfer_callback,
                this,
                kUsbDefaultTimeoutMs
            );
            
            int ret = libusb_submit_transfer(transfer);
            success = ret == 0;
            
            if (success == false)
            {
                SPDLOG_ERROR("Failed to submit transfer ({}).", lookup_libusb_transfer_failure_string(ret));
            }
        }
    }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Exception in step(): {}", e.what());
        _current_step = DeviceStep::Fail;
    }
}

void CarPlayWidget::libusb_event_thread()
{
    SPDLOG_DEBUG("Starting USB event thread.");
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    
    while (_event_thread_should_run == true)
    {
        libusb_handle_events_timeout_completed(nullptr, &tv, nullptr);
    }
    
    SPDLOG_DEBUG("Exiting USB event thread.");
}

void CarPlayWidget::dongle_read_thread()
{
    int transfer_len = 0;
    
    bool expecting_header = true;
    MessageHeader rx_header = {};
    
    SPDLOG_DEBUG("Starting read thread.");
    
    while (_read_thread_should_run == true)
    {
        uint32_t read_size = 0u;
        if (expecting_header == true)
        {
            read_size = MessageHeader::kDataLength;
        }
        else
        {
            read_size = std::min(rx_header.get_message_length(), sizeof(_rx_data));
        }
        
        int ret = libusb_bulk_transfer(
            _device_handle,
            kEndpointInAddress,
            &_rx_data[0],
            read_size,
            &transfer_len,
            kUsbDefaultTimeoutMs);
        
        if ((ret == LIBUSB_ERROR_NO_DEVICE) || (ret == LIBUSB_ERROR_NOT_FOUND))
        {
            SPDLOG_DEBUG("Ending read thread since the device is invalid.");
            break;
        }
        else if (ret == LIBUSB_ERROR_TIMEOUT)  // Timeouts are benign. Maybe?
        {
            continue;
        }
        else if (ret != 0)
        {
            SPDLOG_ERROR("Read failed with {}.", libusb_error_name(ret));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Decode as header.
        if (expecting_header == true)
        {
            if (transfer_len != MessageHeader::kDataLength)
            {
                SPDLOG_ERROR("Expecting a header, but didn't receive {} bytes.", MessageHeader::kDataLength);
                continue;
            }
            
            rx_header = MessageHeader::from_buffer(&_rx_data[0]);
            
            // If we aren't expecting a payload, then assume we are to expect another header.
            expecting_header = rx_header.get_message_length() == 0;
        }
        // Decode as payload based on the header we received previously.
        else
        {
            expecting_header = true;
            
            if (rx_header.get_message_length() != static_cast<size_t>(transfer_len))
            {
                SPDLOG_ERROR("Expecting to receive {} bytes, but actually received {}.",
                    rx_header.get_message_length(),
                    transfer_len);
            }
            else
            {
                decode_dongle_response(rx_header, &_rx_data[0]);
            }
        }
    }
    
    SPDLOG_DEBUG("Exiting read thread.");
}

void CarPlayWidget::heartbeat_thread()
{
    uint8_t buffer[4] = {0u};
    std::vector<uint8_t> heartbeat = Heartbeat(&buffer[0]).serialize();
    int transfer_len = 0;
    
    auto sleep_time = std::chrono::steady_clock::now();
    
    SPDLOG_DEBUG("Starting heartbeat thread.");
    while (_heartbeat_thread_should_run == true)
    {
        int ret = libusb_bulk_transfer(
            _device_handle,
            kEndpointOutAddress,
            &heartbeat[0],
            heartbeat.size(),
            &transfer_len,
            kUsbDefaultTimeoutMs);
        
        if ((ret == LIBUSB_ERROR_NO_DEVICE) || (ret == LIBUSB_ERROR_NOT_FOUND))
        {
            SPDLOG_DEBUG("Ending heartbeat thread since the device is invalid.");
            break;
        }
        else if (ret != 0)
        {
            SPDLOG_ERROR("Heartbeat failed with {}.", libusb_error_name(ret));
        }
        
        sleep_time += std::chrono::milliseconds(kHeartbeatTimeMs);
        std::this_thread::sleep_until(sleep_time);
    }
    
    SPDLOG_DEBUG("Exiting heartbeat thread.");
}

void CarPlayWidget::decode_dongle_response(MessageHeader header, const uint8_t* buffer)
{
    switch (header.get_message_type())
    {
        case (MessageType::Command):
            {
                auto cmd = Command(&buffer[0]);
                SPDLOG_DEBUG("Received Command::{} ({}), length = {}.", command_mapping_to_string(cmd.get_value()),
                    static_cast<uint32_t>(cmd.get_value()), header.get_message_length());
            }
            break;
            
        case (MessageType::VideoData):
            // Video data is passed directly to the integrated decoder
            accept_new_data(&buffer[20], header.get_message_length() - 20);
            break;
            
        case (MessageType::AudioData):
            {
                uint32_t decode_type = read_uint32_t_little_endian(&buffer[0]);
                float volume = static_cast<float>(read_uint32_t_little_endian(&buffer[4]));
                uint32_t audio_type = read_uint32_t_little_endian(&buffer[8]);
                
                if (header.get_message_length() == 13)
                {
                    AudioCommand cmd = static_cast<AudioCommand>(buffer[12]);
                    SPDLOG_DEBUG("Audio Command {}, decode_type = {}, volume = {}, audio_type = {}",
                        audio_command_to_string(cmd),
                        decode_type,
                        volume,
                        audio_type);
                }
                else if (header.get_message_length() == 16)
                {
                    uint32_t volume_duration = read_uint32_t_little_endian(&buffer[12]);
                    SPDLOG_DEBUG("volume_duration = {}, decode_type = {}, volume = {}, audio_type = {}",
                        volume_duration,
                        decode_type,
                        volume,
                        audio_type);
                }
                else
                {
                    // Audio data!
                    if (_audio_ready_callback != nullptr)
                    {
                        // TODO: Re-enable audio.
                        //_audio_ready_callback(&buffer[12], header.get_message_length() - 12);
                    }
                }
            }
            break;
            
        case (MessageType::SoftwareVersion):
            {
                auto cmd = SoftwareVersion(header, &buffer[0]);
                SPDLOG_DEBUG("Dongle software version: {}", cmd.version());
            }
            break;
            
        default:
            SPDLOG_WARN("Received unknown message {}.", msg_type_to_string(header.get_message_type()));
            break;
    }
}

void CarPlayWidget::send_touch_event_internal(TouchAction action, uint32_t x, uint32_t y)
{
    if (_current_step != DeviceStep::Done)
    {
        SPDLOG_WARN("Ignoring touch event (Device step == {}).", device_step_to_string(_current_step));
        return;
    }
    
    // We have to scale from 0....10,000.
    uint32_t scaled_x = static_cast<uint32_t>((static_cast<float>(x) / static_cast<float>(_cfg.width_px)) * 10000.0f);
    uint32_t scaled_y = static_cast<uint32_t>((static_cast<float>(y) / static_cast<float>(_cfg.height_px)) * 10000.0f);
    
    std::vector<uint8_t> usb_request = SendTouch(action, scaled_x, scaled_y).serialize();
    
    int transfer_len = 0;
    
    int ret = libusb_bulk_transfer(_device_handle, kEndpointOutAddress, &usb_request[0], usb_request.size(),
        &transfer_len, kUsbDefaultTimeoutMs);
    if (ret != 0)
    {
        SPDLOG_ERROR("Failed to submit touch transfer ({}).", lookup_libusb_transfer_failure_string(ret));
    }
}

#include "carplay/moc_carplay_widget.cpp"