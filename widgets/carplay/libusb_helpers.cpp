#include "carplay/libusb_helpers.h"

std::string_view lookup_libusb_transfer_failure_string(int ret)
{
    switch (ret)
    {
        case (0): return "NO ERROR";
        case (LIBUSB_ERROR_NO_DEVICE): return "LIBUSB_ERROR_NO_DEVICE";
        case (LIBUSB_ERROR_BUSY): return "LIBUSB_ERROR_BUSY";
        case (LIBUSB_ERROR_NOT_SUPPORTED): return "LIBUSB_ERROR_NOT_SUPPORTED";
        case (LIBUSB_ERROR_INVALID_PARAM): return "LIBUSB_ERROR_INVALID_PARAM";
        default: return "UNKNOWN ERROR";
    }
}

std::string_view lookup_libusb_transfer_status_string(libusb_transfer_status status)
{
    switch (status)
    {
        case (LIBUSB_TRANSFER_COMPLETED): return "LIBUSB_TRANSFER_COMPLETED";
        case (LIBUSB_TRANSFER_ERROR): return "LIBUSB_TRANSFER_ERROR";
        case (LIBUSB_TRANSFER_TIMED_OUT): return "LIBUSB_TRANSFER_TIMED_OUT";
        case (LIBUSB_TRANSFER_CANCELLED): return "LIBUSB_TRANSFER_CANCELLED";
        case (LIBUSB_TRANSFER_STALL): return "LIBUSB_TRANSFER_STALL";
        case (LIBUSB_TRANSFER_NO_DEVICE): return "LIBUSB_TRANSFER_NO_DEVICE";
        case (LIBUSB_TRANSFER_OVERFLOW): return "LIBUSB_TRANSFER_OVERFLOW";
        default: return "UNKNOWN";
    }
}
