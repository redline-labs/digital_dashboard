#include "dongle_driver.h"

#include <libusb/libusb.h>
#include <libusb/version.h>

#include <spdlog/spdlog.h>

#include <array>

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

static int hotplug_callback(libusb_context* /*ctx*/, libusb_device* /*device*/, libusb_hotplug_event event, void* /*user_data*/)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
    {
        SPDLOG_INFO("Attached!");
    }
    else
    {
        SPDLOG_INFO("Removed!");
    }

    return 0;
}


DongleDriver::DongleDriver(bool libusb_debug):
  _device_handle{nullptr},
  _should_run{true}
{
    SPDLOG_DEBUG("Using libusb {}", libusb_version());

    libusb_init_context(nullptr, nullptr, /*num_options=*/0);

    if (true == libusb_debug)
    {
        libusb_set_log_cb(nullptr, &libusb_log, LIBUSB_LOG_CB_GLOBAL);
        libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }

    // Register hotplug callback.
    libusb_hotplug_register_callback(
        nullptr,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE,
        kUsbVid,
        kUsbPid,
        LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_callback,
        nullptr,
        &_hotplug_callback_handle
    );

    _event_thread = std::thread(std::bind(&DongleDriver::libusb_event_thread, this));
}

void DongleDriver::stop()
{
    _should_run = false;
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


std::string_view DongleDriver::libusb_version()
{
    return {STR(LIBUSB_MAJOR) "." STR(LIBUSB_MINOR) "." STR(LIBUSB_MICRO)};
}

DongleDriver::~DongleDriver()
{
    libusb_hotplug_deregister_callback(nullptr, _hotplug_callback_handle);

    stop();

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
    SPDLOG_INFO("Starting USB event thread.");

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    while (_should_run == true)
    {
        libusb_handle_events_timeout_completed(nullptr, &tv, nullptr);
    }

    SPDLOG_INFO("Exiting USB event thread.");
}