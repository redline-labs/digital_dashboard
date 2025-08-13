#ifndef LIBUSB_HELPERS_H_
#define LIBUSB_HELPERS_H_

#include <libusb.h>

#include <cstdint>
#include <string_view>

std::string_view lookup_libusb_transfer_failure_string(int ret);
std::string_view lookup_libusb_transfer_status_string(libusb_transfer_status status);

#endif  // LIBUSB_HELPERS_H_