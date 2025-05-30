#include "dongle_driver.h"
#include "dongle_config_file.h"

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
        SPDLOG_INFO("Dongle was removed!");
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

constexpr std::string_view device_step_to_string(DeviceStep step)
{
    switch (step)
    {
        case (DeviceStep::Init):
            return "Init";
        case (DeviceStep::SendDPI):
            return "SendDPI";
        case (DeviceStep::SendOpen):
            return "SendOpen";
        case (DeviceStep::SendNightMode):
            return "SendNightMode";
        case (DeviceStep::SendDriveHand):
            return "SendDriveHand";
        case (DeviceStep::SendChargeMode):
            return "SendChargeMode";
        case (DeviceStep::SendBoxName):
            return "SendBoxName";
        case (DeviceStep::SendBoxSettings):
            return "SendBoxSettings";
        case (DeviceStep::SendWiFiEnable):
            return "SendWiFiEnable";
        case (DeviceStep::SendWiFiType):
            return "SendWiFiType";
        case (DeviceStep::SendMicType):
            return "SendMicType";
        case (DeviceStep::SendAudioTransferMode):
            return "SendAudioTransferMode";
        case (DeviceStep::SendWiFiConnect):
            return "SendWiFiConnect";
        case (DeviceStep::Done):
            return "Done";
        case (DeviceStep::Fail):
            return "Fail";

        default:
            return "UNKNOWN";
    }
}

static void libusb_transfer_callback(struct libusb_transfer *transfer)
{


    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        static_cast<DongleDriver*>(transfer->user_data)->step();
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
}

void DongleDriver::register_frame_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb)
{
    _frame_ready_callback = cb;
}

void DongleDriver::register_audio_ready_callback(std::function<void(const uint8_t* buffer, uint32_t buffer_len)> cb)
{
    _audio_ready_callback = cb;
}


void DongleDriver::stop()
{
    //_should_run = false;
    _current_step = DeviceStep::Init;

    // Stop the read thread.
    _read_thread_should_run = false;
    if (_read_thread.joinable() == true)
    {
        _read_thread.join();
    }

    // Stop the heartbeat thread.
    _heartbeat_thread_should_run = false;
    if (_heartbeat_thread.joinable() == true)
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

            SPDLOG_DEBUG("Attached to dongle.");
            // Fall through;

        case (DeviceStep::SendDPI):
            SPDLOG_DEBUG("Sending config (DPI).");
            _usb_request = SendNumber(DongleConfigFile::DPI, _app_cfg.dpi).serialize();
            _current_step = DeviceStep::SendOpen;  // Next step on success;
            break;

        case (DeviceStep::SendOpen):
            SPDLOG_DEBUG("Sending config (Open).");
            _usb_request = SendOpen(_app_cfg).serialize();
            _current_step = DeviceStep::SendNightMode;  // Next step on success;
            break;

        case (DeviceStep::SendNightMode):
            SPDLOG_DEBUG("Sending config (Night Mode).");
            _usb_request = SendBoolean(DongleConfigFile::NightMode, _app_cfg.night_mode).serialize();
            _current_step = DeviceStep::SendDriveHand;  // Next step on success;
            break;

        case (DeviceStep::SendDriveHand):
            SPDLOG_DEBUG("Sending config (drive side).");
            _usb_request = SendNumber(DongleConfigFile::HandDriveMode, static_cast<uint32_t>(_app_cfg.drive_type)).serialize();
            _current_step = DeviceStep::SendChargeMode;  // Next step on success;
            break;

        case (DeviceStep::SendChargeMode):
            SPDLOG_DEBUG("Sending config (charge mode).");
            _usb_request = SendBoolean(DongleConfigFile::ChargeMode, true).serialize();
            _current_step = DeviceStep::SendBoxName;  // Next step on success;
            break;

        case (DeviceStep::SendBoxName):
            SPDLOG_DEBUG("Sending config (box name).");
            _usb_request = SendString(DongleConfigFile::BoxName, _app_cfg.box_name).serialize();
            _current_step = DeviceStep::SendBoxSettings;  // Next step on success;
            break;

        case (DeviceStep::SendBoxSettings):
            SPDLOG_DEBUG("Sending config (box settings).");
            _usb_request = SendBoxSettings(_app_cfg).serialize();
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
            _heartbeat_thread_should_run = true;
            _heartbeat_thread = std::thread(std::bind(&DongleDriver::heartbeat_thread, this));
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
}



std::string_view DongleDriver::libusb_version()
{
    return {STR(LIBUSB_MAJOR) "." STR(LIBUSB_MINOR) "." STR(LIBUSB_MICRO)};
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
}

void DongleDriver::libusb_event_thread()
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

void DongleDriver::read_thread()
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


static uint32_t read_uint32_t_little_endian(const uint8_t* buffer)
{
    return
        (static_cast<uint32_t>(buffer[0]) <<  0) |
        (static_cast<uint32_t>(buffer[1]) <<  8) |
        (static_cast<uint32_t>(buffer[2]) << 16) |
        (static_cast<uint32_t>(buffer[3]) << 24);
}


void DongleDriver::decode_dongle_response(MessageHeader header, const uint8_t* buffer)
{
    switch (header.get_message_type())
    {
        case (MessageType::Command):
            {
                auto cmd = Command(&buffer[0]);
                SPDLOG_DEBUG("Received Command::{} ({}).", command_mapping_to_string(cmd.get_value()),
                    static_cast<uint32_t>(cmd.get_value()));
            }
            break;

        case (MessageType::VideoData):
            // TODO: Add bounds checks here to make sure we're not gonna shoot ourselves in the foot.
            //(self.width, self.height, self.flags, self.unknown1, self.unknown2) = struct.unpack("<LLLLL", data[:20])
            /*{
                uint32_t rx_width = read_uint32_t_little_endian(&buffer[0]);
                uint32_t rx_height = read_uint32_t_little_endian(&buffer[4]);
                uint32_t rx_flags = read_uint32_t_little_endian(&buffer[8]);
                uint32_t rx_unknown1 = read_uint32_t_little_endian(&buffer[12]);
                uint32_t rx_unknown2 = read_uint32_t_little_endian(&buffer[16]);

                SPDLOG_DEBUG("rx_width = {}, rx_height = {}, rx_flags = {}, rx_unknown1 = {}, rx_unknown2 = {}",
                    rx_width, rx_height, rx_flags, rx_unknown1, rx_unknown2);
            }*/

            if (_frame_ready_callback != nullptr)
            {
                _frame_ready_callback(&buffer[20], header.get_message_length() - 20);
            }
            break;

        case (MessageType::AudioData):
            {
                //uint32_t decode_type = read_uint32_t_little_endian(&buffer[0]);
                //float volume = static_cast<float>(read_uint32_t_little_endian(&buffer[4]));
                //uint32_t audio_type = read_uint32_t_little_endian(&buffer[8]);

                if (header.get_message_length() == 13)
                {
                    AudioCommand cmd = static_cast<AudioCommand>(buffer[12]);
                    SPDLOG_DEBUG("Audio Command {}", audio_command_to_string(cmd));
                }
                else if (header.get_message_length() == 16)
                {
                    uint32_t volume_duration = read_uint32_t_little_endian(&buffer[12]);
                    SPDLOG_DEBUG("volume_duration = {}", volume_duration);
                }
                else
                {
                    // Audio data!
                    if (_audio_ready_callback != nullptr)
                    {
                        _audio_ready_callback(&buffer[12], header.get_message_length() - 12);
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

void DongleDriver::send_touch_event(TouchAction action, uint32_t x, uint32_t y)
{
    if (_current_step != DeviceStep::Done)
    {
        SPDLOG_WARN("Ignoring touch event (Device step == {}).", device_step_to_string(_current_step));
        return;
    }

    // We have to scale from 0....10,000.
    uint32_t scaled_x = static_cast<uint32_t>((static_cast<float>(x) / static_cast<float>(_app_cfg.width_px)) * 10000.0f);
    uint32_t scaled_y = static_cast<uint32_t>((static_cast<float>(y) / static_cast<float>(_app_cfg.height_px)) * 10000.0f);

    std::vector<uint8_t> usb_request = SendTouch(action, scaled_x, scaled_y).serialize();

    int transfer_len = 0;

    int ret = libusb_bulk_transfer(_device_handle, kEndpointOutAddress, &usb_request[0], usb_request.size(),
        &transfer_len, kUsbDefaultTimeoutMs);
    if (ret != 0)
    {
        SPDLOG_ERROR("Failed to submit touch transfer ({}).", lookup_libusb_transfer_failure_string(ret));
    }
}
