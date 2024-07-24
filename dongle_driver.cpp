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


DongleDriver::DongleDriver(bool libusb_debug)
{
    libusb_init_context(nullptr, nullptr, /*num_options=*/0);

    if (true == libusb_debug)
    {
        libusb_set_log_cb(nullptr, &libusb_log, LIBUSB_LOG_CB_GLOBAL);
        libusb_set_option(nullptr, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }
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
    if (_device_handle != nullptr)
    {
        libusb_release_interface(_device_handle, kInterfaceNumber);
        libusb_close(_device_handle);
    }

    libusb_exit(nullptr);
}