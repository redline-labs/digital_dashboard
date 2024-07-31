#include "dongle_driver.h"
#include "dongle_config_file.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <libusb/libusb.h>
#include <libusb/version.h>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <array>
#include <chrono>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)


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

static int hotplug_callback(libusb_context* /*ctx*/, libusb_device* /*device*/, libusb_hotplug_event event, void* driver_instance)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
    {
        static_cast<DongleDriver*>(driver_instance)->step();
    }
    else
    {
        SPDLOG_INFO("Removed!");
        static_cast<DongleDriver*>(driver_instance)->stop();
    }

    return 0;
}

static std::string_view lookup_libusb_transfer_failure_string(int ret)
{
    switch (ret)
    {
        case (0):
            return "NO ERROR";

        case (LIBUSB_ERROR_NO_DEVICE):
            return "LIBUSB_ERROR_NO_DEVICE";

        case (LIBUSB_ERROR_BUSY):
            return "LIBUSB_ERROR_BUSY";

        case (LIBUSB_ERROR_NOT_SUPPORTED):
            return "LIBUSB_ERROR_NOT_SUPPORTED";

        case (LIBUSB_ERROR_INVALID_PARAM):
            return "LIBUSB_ERROR_INVALID_PARAM";

        default:
            return "UNKNOWN ERROR";
    }
}

static std::string_view lookup_libusb_transfer_status_string(libusb_transfer_status status)
{
    switch (status)
    {
        case (LIBUSB_TRANSFER_COMPLETED):
            return "LIBUSB_TRANSFER_COMPLETED";

        case (LIBUSB_TRANSFER_ERROR):
            return "LIBUSB_TRANSFER_ERROR";

        case (LIBUSB_TRANSFER_TIMED_OUT):
            return "LIBUSB_TRANSFER_TIMED_OUT";

        case (LIBUSB_TRANSFER_CANCELLED):
            return "LIBUSB_TRANSFER_CANCELLED";

        case (LIBUSB_TRANSFER_STALL):
            return "LIBUSB_TRANSFER_STALL";

        case (LIBUSB_TRANSFER_NO_DEVICE):
            return "LIBUSB_TRANSFER_NO_DEVICE";

        case (LIBUSB_TRANSFER_OVERFLOW):
            return "LIBUSB_TRANSFER_OVERFLOW";

        default:
            return "UNKNOWN";
    }
}

static void libusb_transfer_callback(struct libusb_transfer *transfer)
{
    SPDLOG_INFO("{} : transferred {} bytes (expected {}).",
        lookup_libusb_transfer_status_string(transfer->status),
        transfer->actual_length,
        transfer->length);

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        static_cast<DongleDriver*>(transfer->user_data)->step();
    }

    libusb_free_transfer(transfer);
}


DongleDriver::DongleDriver(app_config_t cfg, bool libusb_debug):
  _app_cfg{cfg},
  _device_handle{nullptr},
  _current_step{DeviceStep::Init},
  _event_thread_should_run{true},
  _read_thread_should_run{true},
  _heartbeat_thread_should_run{true},
  _frame_ready_callback{nullptr}
{
    SPDLOG_DEBUG("Using libusb {}.", libusb_version());
    SPDLOG_DEBUG("Using libavcodec {}.", libavcodec_version());

    libusb_init_context(nullptr, nullptr, /*num_options=*/0);

    if (true == libusb_debug)
    {
        libusb_set_log_cb(nullptr, &libusb_log, LIBUSB_LOG_CB_GLOBAL);
        libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }

    _event_thread = std::thread(std::bind(&DongleDriver::libusb_event_thread, this));

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


    // Get set up for libavcodec.
    _pkt = av_packet_alloc();
    if (_pkt == nullptr)
    {
        SPDLOG_ERROR("Failed to find allocate packet.");
    }

    _codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (_codec == nullptr)
    {
        SPDLOG_ERROR("Failed to find libavcodec codec.");
    }

    _parser = av_parser_init(_codec->id);
    if (_parser == nullptr)
    {
        SPDLOG_ERROR("Failed to init parser.");
    }

     _codec_context = avcodec_alloc_context3(_codec);
    if (_codec_context == nullptr)
    {
        SPDLOG_ERROR("Failed to init codec context.");
    }

    if (avcodec_open2(_codec_context, _codec, nullptr) < 0)
    {
        SPDLOG_ERROR("Could not open codec.");
    }

    _frame = av_frame_alloc();
    if (_frame == nullptr)
    {
        SPDLOG_ERROR("Could not allocate frame.");
    }
}

void DongleDriver::register_frame_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb)
{
    _frame_ready_callback = cb;
}


void DongleDriver::stop()
{
    //_should_run = false;
    _current_step = DeviceStep::Init;

    // Stop the read thread.
    _read_thread_should_run = false;
    if (_read_thread.joinable() == true)
    {
        SPDLOG_DEBUG("Stopping read thread.");
        _read_thread.join();
    }

    // Stop the heartbeat thread.
    _heartbeat_thread_should_run = false;
    if (_heartbeat_thread.joinable() == true)
    {
        SPDLOG_DEBUG("Stopping heartbeat thread.");
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


bool DongleDriver::find_dongle()
{
    _device_handle = libusb_open_device_with_vid_pid(nullptr, kUsbVid, kUsbPid);
    bool success = (_device_handle != nullptr);

    if (success == true)
    {
        int result = libusb_claim_interface(_device_handle, kInterfaceNumber);
        success = (result == 0);
    }

    return success;
}


void DongleDriver::step()
{
    std::vector<uint8_t> usb_request = {};

    // Slow our roll so we don't bomb the dongle all at once.
    std::this_thread::sleep_for(std::chrono::milliseconds(kUsbConfigDelayMs));

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
            _read_thread = std::thread(std::bind(&DongleDriver::read_thread, this));

            SPDLOG_INFO("Attached to dongle.");
            // Fall through;

        case (DeviceStep::SendDPI):
            SPDLOG_INFO("Sending config (DPI).");
            usb_request = SendNumber(DongleConfigFile::DPI, _app_cfg.dpi).serialize();
            _current_step = DeviceStep::SendOpen;  // Next step on success;
            break;

        case (DeviceStep::SendOpen):
            SPDLOG_INFO("Sending config (Open).");
            usb_request = SendOpen(_app_cfg).serialize();
            _current_step = DeviceStep::SendNightMode;  // Next step on success;
            break;

        case (DeviceStep::SendNightMode):
            SPDLOG_INFO("Sending config (Night Mode).");
            usb_request = SendBoolean(DongleConfigFile::NightMode, _app_cfg.night_mode).serialize();
            _current_step = DeviceStep::SendDriveHand;  // Next step on success;
            break;

        case (DeviceStep::SendDriveHand):
            SPDLOG_INFO("Sending config (drive side).");
            usb_request = SendNumber(DongleConfigFile::HandDriveMode, static_cast<uint32_t>(_app_cfg.drive_type)).serialize();
            _current_step = DeviceStep::SendChargeMode;  // Next step on success;
            break;

        case (DeviceStep::SendChargeMode):
            SPDLOG_INFO("Sending config (charge mode).");
            usb_request = SendBoolean(DongleConfigFile::ChargeMode, true).serialize();
            _current_step = DeviceStep::SendBoxName;  // Next step on success;
            break;

        case (DeviceStep::SendBoxName):
            SPDLOG_INFO("Sending config (box name).");
            usb_request = SendString(DongleConfigFile::BoxName, _app_cfg.box_name).serialize();
            _current_step = DeviceStep::SendBoxSettings;  // Next step on success;
            break;

        case (DeviceStep::SendBoxSettings):
            SPDLOG_INFO("Sending config (box settings).");
            usb_request = SendBoxSettings(_app_cfg).serialize();
            _current_step = DeviceStep::SendWiFiEnable;  // Next step on success;
            break;

        case (DeviceStep::SendWiFiEnable):
            SPDLOG_INFO("Sending config (WiFi enable).");
            usb_request = Command(CommandMapping::WifiEnable).serialize();
            _current_step = DeviceStep::SendWiFiType;  // Next step on success;
            break;

        case (DeviceStep::SendWiFiType):
            SPDLOG_INFO("Sending config (WiFi type).");
            usb_request = Command(CommandMapping::Wifi24g).serialize();
            _current_step = DeviceStep::SendMicType;  // Next step on success;
            break;

        case (DeviceStep::SendMicType):
            SPDLOG_INFO("Sending config (Mic type).");
            usb_request = Command(CommandMapping::Mic).serialize();
            _current_step = DeviceStep::SendAudioTransferMode;  // Next step on success;
            break;

        case (DeviceStep::SendAudioTransferMode):
            SPDLOG_INFO("Sending config (audio transfer mode).");
            usb_request = Command(CommandMapping::AudioTransferOff).serialize();
            _current_step = DeviceStep::Done;  // Next step on success;
            break;

        case (DeviceStep::Done):
            SPDLOG_INFO("Hooray!");

            _heartbeat_thread_should_run = true;
            _heartbeat_thread = std::thread(std::bind(&DongleDriver::heartbeat_thread, this));
            break;

        case (DeviceStep::Fail):
            SPDLOG_ERROR("FUCK!");
            break;

        default:
            SPDLOG_ERROR("Invalid initialization.");
            break;
    }


    if (usb_request.empty() == false)
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
                &usb_request[0],
                usb_request.size(),
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
}



std::string_view DongleDriver::libusb_version()
{
    return {STR(LIBUSB_MAJOR) "." STR(LIBUSB_MINOR) "." STR(LIBUSB_MICRO)};
}

uint32_t DongleDriver::libavcodec_version()
{
    return avcodec_version();
}

DongleDriver::~DongleDriver()
{
    libusb_hotplug_deregister_callback(nullptr, _hotplug_callback_handle);

    stop();

    _event_thread_should_run = false;
    if (_event_thread.joinable() == true)
    {
        _event_thread.join();
    }

    if (_device_handle != nullptr)
    {
        libusb_release_interface(_device_handle, kInterfaceNumber);
        libusb_close(_device_handle);
    }

    libusb_exit(nullptr);

    av_parser_close(_parser);
    avcodec_free_context(&_codec_context);
    av_frame_free(&_frame);
    av_packet_free(&_pkt);
}

void DongleDriver::libusb_event_thread()
{
    SPDLOG_INFO("Starting USB event thread.");

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    while (_event_thread_should_run == true)
    {
        libusb_handle_events_timeout_completed(nullptr, &tv, nullptr);
    }

    SPDLOG_INFO("Exiting USB event thread.");
}

void DongleDriver::read_thread()
{
    uint8_t rx_data[64u * 1024u] = {};
    int transfer_len = 0;

    bool expecting_header = true;
    MessageHeader rx_header = {};

    SPDLOG_INFO("Starting read thread.");

    while (_read_thread_should_run == true)
    {
        uint32_t read_size = 0u;
        if (expecting_header == true)
        {
            read_size = MessageHeader::kDataLength;
        }
        else
        {
            read_size = std::min(rx_header.get_message_length(), sizeof(rx_data));
        }

        int ret = libusb_bulk_transfer(
            _device_handle,
            kEndpointInAddress,
            &rx_data[0],
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

            rx_header = MessageHeader::from_buffer(&rx_data[0]);

            // If we aren't expecting a payload, then assume we are to expect another header.
            expecting_header = rx_header.get_message_length() == 0;
        }
        // Decode as payload based on the header we received previously.
        else
        {
            expecting_header = true;

            if (rx_header.get_message_length() != transfer_len)
            {
                SPDLOG_ERROR("Expecting to receive {} bytes, but actually received {}.",
                    rx_header.get_message_length(),
                    transfer_len);
            }
            else
            {
                decode_dongle_response(rx_header, &rx_data[0]);
            }
        }

    }

    SPDLOG_INFO("Exiting read thread.");
}


//Save RGB image as PPM file format
static void ppm_save(unsigned char* buf, int wrap, int xsize, int ysize)
{
    FILE* f;
    int i;

    f = fopen("output.ppm", "wb");
    fprintf(f, "P6\n%d %d\n%d\n", xsize, ysize, 255);

    for (i = 0; i < ysize; i++)
    {
        fwrite(buf + i * wrap, 1, xsize*3, f);
    }

    fclose(f);
}

void DongleDriver::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    struct SwsContext* sws_ctx = NULL;

    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        SPDLOG_ERROR("Error sending a packet for decoding.");
        return;
    }


    //Create SWS Context for converting from decode pixel format (like YUV420) to RGB
    ////////////////////////////////////////////////////////////////////////////
    sws_ctx = sws_getContext(dec_ctx->width,
                             dec_ctx->height,
                             dec_ctx->pix_fmt,
                             dec_ctx->width,
                             dec_ctx->height,
                             AV_PIX_FMT_RGB24,
                             SWS_BICUBIC,
                             NULL,
                             NULL,
                             NULL);

    if (sws_ctx == nullptr)
    {
        SPDLOG_ERROR("Failed to get SWS context.");
        return;
    }

    AVFrame* pRGBFrame = av_frame_alloc();

    pRGBFrame->format = AV_PIX_FMT_RGB24;
    pRGBFrame->width = dec_ctx->width;
    pRGBFrame->height = dec_ctx->height;

    int sts = av_frame_get_buffer(pRGBFrame, 0);

    if (sts < 0)
    {
        SPDLOG_ERROR("Failed to AV frame buffer.");
        return;  //Error!
    }


    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return;
        }
        else if (ret < 0)
        {
            SPDLOG_ERROR("Error during decoding");
            return;
        }

        //Convert from input format (e.g YUV420) to RGB and save to PPM:
        ////////////////////////////////////////////////////////////////////////////
        sts = sws_scale(sws_ctx,                //struct SwsContext* c,
                        frame->data,            //const uint8_t* const srcSlice[],
                        frame->linesize,        //const int srcStride[],
                        0,                      //int srcSliceY,
                        frame->height,          //int srcSliceH,
                        pRGBFrame->data,        //uint8_t* const dst[],
                        pRGBFrame->linesize);   //const int dstStride[]);

        if (sts != frame->height)
        {
            SPDLOG_ERROR("STS does not match frame height.");
            return;  //Error!
        }

        /* the picture is allocated by the decoder. no need to
           free it */
        //ppm_save(pRGBFrame->data[0], pRGBFrame->linesize[0], pRGBFrame->width, pRGBFrame->height);
        if (nullptr != _frame_ready_callback)
        {
            _frame_ready_callback(
                pRGBFrame->data[0],
                av_image_get_buffer_size(AV_PIX_FMT_RGB24, pRGBFrame->width, pRGBFrame->height, 1)
            );
        }
    }

    //Free
    sws_freeContext(sws_ctx);
    av_frame_free(&pRGBFrame);
}


void DongleDriver::decode_dongle_response(MessageHeader header, const uint8_t* buffer)
{
    switch (header.get_message_type())
    {
        case (MessageType::Command):
            {
                auto cmd = Command(&buffer[0]);
                SPDLOG_DEBUG("Received Command::{}.", command_mapping_to_string(cmd.get_value()));
            }
            break;

        case (MessageType::VideoData):
            SPDLOG_DEBUG("Received video data. {} bytes.", header.get_message_length());
            {
                const uint8_t* data = buffer;
                uint32_t data_len = header.get_message_length();

                while (data_len > 0)
                {
                    int ret = av_parser_parse2(
                        _parser,
                        _codec_context,
                        &_pkt->data,
                        &_pkt->size,
                        data,
                        data_len,
                        AV_NOPTS_VALUE,
                        AV_NOPTS_VALUE,
                        0);

                    if (ret < 0)
                    {
                        SPDLOG_ERROR("Failed to parse data.");
                        break;
                    }
                    else
                    {
                        data += ret;
                        data_len -= ret;
                    }

                    if (_pkt->size)
                    {
                        decode(_codec_context, _frame, _pkt);
                    }
                }
            }
            break;

        default:
            SPDLOG_DEBUG("Received message {}.", msg_type_to_string(header.get_message_type()));
            break;
    }
}




void DongleDriver::heartbeat_thread()
{
    uint8_t buffer[4] = {0u};
    std::vector<uint8_t> heartbeat = Heartbeat(&buffer[0]).serialize();
    int transfer_len = 0;

    auto sleep_time = std::chrono::steady_clock::now();

    SPDLOG_INFO("Starting heartbeat thread.");
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

    SPDLOG_INFO("Exiting heartbeat thread.");
}
