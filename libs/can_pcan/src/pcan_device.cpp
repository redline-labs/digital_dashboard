// SPDX-License-Identifier: GPL-3.0-or-later

#include "pcan_device.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace can::pcan
{
namespace
{

// A bulk read buffer big enough for a full high-speed transfer of records.
constexpr size_t kReadBufferSize = 2048;

// PEAK's vendor requests go to the "other" recipient rather than the device or
// an interface. Composed as a byte because libusb's request-type,
// recipient and direction constants are three separate enums.
constexpr uint8_t kVendorRequestIn = static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR)
    | static_cast<uint8_t>(LIBUSB_RECIPIENT_OTHER) | static_cast<uint8_t>(LIBUSB_ENDPOINT_IN);
constexpr uint8_t kVendorRequestOut = static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR)
    | static_cast<uint8_t>(LIBUSB_RECIPIENT_OTHER) | static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT);

Error from_libusb(int code, std::string what)
{
    Error::Kind kind = Error::Kind::Io;
    switch (code)
    {
    case LIBUSB_ERROR_ACCESS:
        kind = Error::Kind::PermissionDenied;
        break;
    case LIBUSB_ERROR_BUSY:
        kind = Error::Kind::Busy;
        break;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        kind = Error::Kind::NotFound;
        break;
    case LIBUSB_ERROR_NOT_SUPPORTED:
        kind = Error::Kind::Unsupported;
        break;
    default:
        break;
    }

    return Error { kind, fmt::format("{}: {}", what, libusb_strerror(static_cast<libusb_error>(code))),
                   code };
}

} // namespace

PcanDevice::PcanDevice(libusb_device_handle* handle, uint16_t productId, uint8_t usbInterface,
                       const PcanOptions& options)
    : handle_(handle)
    , productId_(productId)
    , usbInterface_(usbInterface)
    , options_(options)
{
}

PcanDevice::~PcanDevice()
{
    stop_reader();

    if (handle_ != nullptr)
    {
        // Tell the device the driver is going away, so it stops producing
        // records into a pipe nobody is reading.
        auto payload = encode_driver_loaded(false);
        libusb_control_transfer(handle_,
                                kVendorRequestOut,
                                kRequestFunction, kFunctionDriverLoaded, 0, payload.data(),
                                static_cast<uint16_t>(payload.size()), options_.writeTimeoutMs);

        libusb_release_interface(handle_, usbInterface_);
#if defined(__linux__)
        if (detachedKernelDriver_)
        {
            // Put it back, so unplugging and replugging is not required to get
            // the socketcan interface back.
            libusb_attach_kernel_driver(handle_, usbInterface_);
        }
#endif
        libusb_close(handle_);
        handle_ = nullptr;
    }
}

Result<std::shared_ptr<PcanDevice>> PcanDevice::open(libusb_context* context,
                                                     libusb_device* device, uint16_t productId,
                                                     uint8_t usbInterface,
                                                     const PcanOptions& options)
{
    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(device, &handle);
    if (rc != LIBUSB_SUCCESS)
    {
        auto error = from_libusb(rc, "cannot open the PCAN adapter");
        if (error.kind == Error::Kind::PermissionDenied)
        {
            error.message +=
                " -- on Linux this usually means a udev rule is missing for vendor 0x0c72";
        }
        return std::unexpected(error);
    }

    (void)context;
    auto owned = std::shared_ptr<PcanDevice>(
        new PcanDevice(handle, productId, usbInterface, options));

#if defined(__linux__)
    // The in-tree peak_usb driver claims these adapters at plug-in. Taking the
    // device away from it removes the socketcan interface it created, which is
    // a surprising thing to do to a machine, so it is opt-in.
    const int active = libusb_kernel_driver_active(handle, usbInterface);
    if (active == 1)
    {
        if (!options.detachKernelDriver)
        {
            return busy(fmt::format(
                "the kernel's peak_usb driver holds interface {} of this adapter. On Linux use "
                "the socketcan backend instead ('socketcan:canX'), or pass the option to detach "
                "the kernel driver -- which will remove the socketcan interface it created",
                usbInterface));
        }
        rc = libusb_detach_kernel_driver(handle, usbInterface);
        if (rc != LIBUSB_SUCCESS)
        {
            return std::unexpected(from_libusb(rc, "cannot detach the kernel driver"));
        }
        owned->detachedKernelDriver_ = true;
        SPDLOG_WARN("[pcan] detached the kernel driver from interface {}; its socketcan "
                    "interface is gone until this process exits",
                    usbInterface);
    }
#endif

    rc = libusb_claim_interface(handle, usbInterface);
    if (rc != LIBUSB_SUCCESS)
    {
        auto error = from_libusb(rc, fmt::format("cannot claim interface {}", usbInterface));
        if (error.kind == Error::Kind::Busy)
        {
            error.message += " -- something else has this adapter open";
        }
        return std::unexpected(error);
    }

    // Ask the device what it is. This also proves the control path works
    // before anything is committed to the bulk endpoints.
    std::array<uint8_t, 64> info {};
    rc = libusb_control_transfer(
        handle, kVendorRequestIn,
        kRequestInfo, kInfoFirmware, 0, info.data(), static_cast<uint16_t>(info.size()),
        options.writeTimeoutMs);
    if (rc < 0)
    {
        return std::unexpected(from_libusb(rc, "cannot read the adapter's firmware info"));
    }

    auto firmware = decode_firmware_info(std::span(info.data(), static_cast<size_t>(rc)));
    if (!firmware.has_value())
    {
        return std::unexpected(firmware.error());
    }
    owned->firmware_ = *firmware;

    // Without this the adapter stays idle and never produces a record.
    auto payload = encode_driver_loaded(true);
    rc = libusb_control_transfer(
        handle, kVendorRequestOut,
        kRequestFunction, kFunctionDriverLoaded, 0, payload.data(),
        static_cast<uint16_t>(payload.size()), options.writeTimeoutMs);
    if (rc < 0)
    {
        return std::unexpected(from_libusb(rc, "cannot tell the adapter a driver is loaded"));
    }

    // 0xFFFFFFFF is erased flash, not a serial number: PEAK ships these
    // adapters with the field unprogrammed and printing 4294967295 invites
    // someone to write it into a config file as 'pcan:4294967295'.
    const std::string serial = owned->firmware_.serialNumber == 0xFFFFFFFFu
        ? std::string("unset")
        : std::to_string(owned->firmware_.serialNumber);
    owned->description_ = fmt::format("{} (serial {}, firmware {})", product_name(productId),
                                      serial, owned->firmware_.firmwareVersionString());

    SPDLOG_INFO("[pcan] opened {} interface {}", owned->description_, usbInterface);

    owned->start_reader();
    return owned;
}

void PcanDevice::attach(uint8_t localChannel, RecordSink* sink)
{
    if (localChannel < sinks_.size())
    {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sinks_[localChannel] = sink;
    }
}

void PcanDevice::detach(uint8_t localChannel)
{
    if (localChannel < sinks_.size())
    {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sinks_[localChannel] = nullptr;
    }
}

Result<void> PcanDevice::send_commands(std::vector<uint8_t> buffer)
{
    finish_command_buffer(buffer);

    std::lock_guard<std::mutex> lock(writeMutex_);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, firmware_.commandOutEndpoint, buffer.data(),
                                        static_cast<int>(buffer.size()), &transferred,
                                        options_.writeTimeoutMs);
    if (rc != LIBUSB_SUCCESS)
    {
        return std::unexpected(from_libusb(rc, "cannot send a command to the adapter"));
    }
    if (static_cast<size_t>(transferred) != buffer.size())
    {
        return io_error(fmt::format("a command transfer sent {} of {} bytes", transferred,
                                    buffer.size()));
    }
    return {};
}

Result<void> PcanDevice::send_frames(uint8_t localChannel, std::span<const uint8_t> buffer)
{
    if (localChannel >= kChannelsPerInterface)
    {
        return invalid_argument(fmt::format("channel {} is not on this interface", localChannel));
    }

    const uint8_t endpoint = firmware_.dataOutEndpoint[localChannel];

    std::lock_guard<std::mutex> lock(writeMutex_);
    int transferred = 0;
    const int rc = libusb_bulk_transfer(handle_, endpoint,
                                        const_cast<uint8_t*>(buffer.data()),
                                        static_cast<int>(buffer.size()), &transferred,
                                        options_.writeTimeoutMs);
    if (rc != LIBUSB_SUCCESS)
    {
        return std::unexpected(from_libusb(rc, "cannot send a frame to the adapter"));
    }
    if (static_cast<size_t>(transferred) != buffer.size())
    {
        return io_error(
            fmt::format("a frame transfer sent {} of {} bytes", transferred, buffer.size()));
    }
    return {};
}

void PcanDevice::start_reader()
{
    reading_ = true;
    reader_ = std::thread([this] { reader_loop(); });
}

void PcanDevice::stop_reader()
{
    if (reader_.joinable())
    {
        reading_ = false;
        // The read has a timeout, so the thread notices within one of those
        // rather than needing the transfer cancelled out from under it.
        reader_.join();
    }
}

void PcanDevice::reader_loop()
{
    std::vector<uint8_t> buffer(kReadBufferSize);

    while (reading_)
    {
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, firmware_.dataInEndpoint, buffer.data(),
                                            static_cast<int>(buffer.size()), &transferred,
                                            options_.readTimeoutMs);

        if (rc == LIBUSB_ERROR_TIMEOUT)
        {
            // A quiet bus produces no transfers. Normal.
            continue;
        }
        if (rc == LIBUSB_ERROR_NO_DEVICE)
        {
            SPDLOG_ERROR("[pcan] the adapter was unplugged");
            break;
        }
        if (rc != LIBUSB_SUCCESS)
        {
            SPDLOG_WARN("[pcan] bulk read failed: {}",
                        libusb_strerror(static_cast<libusb_error>(rc)));
            continue;
        }
        if (transferred <= 0)
        {
            continue;
        }

        auto decoded = decode_records(std::span(buffer.data(), static_cast<size_t>(transferred)));
        if (decoded.error.has_value())
        {
            // The records before the bad one are still good and are still
            // dispatched below; only the rest of this transfer is lost.
            SPDLOG_WARN("[pcan] {}", to_string(*decoded.error));
        }

        for (const auto& record : decoded.records)
        {
            RecordSink* sink = nullptr;
            {
                std::lock_guard<std::mutex> lock(sinkMutex_);
                if (record.channel < sinks_.size())
                {
                    sink = sinks_[record.channel];
                }
            }
            // A record for a channel nobody opened is not an error: the other
            // channel of a Pro FD is live whether or not we asked for it.
            if (sink != nullptr)
            {
                sink->on_record(record);
            }
        }
    }
}

} // namespace can::pcan
