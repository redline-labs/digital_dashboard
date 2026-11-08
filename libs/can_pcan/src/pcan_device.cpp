// SPDX-License-Identifier: GPL-3.0-or-later

#include "pcan_device.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace can::pcan
{
namespace
{

// A bulk read buffer big enough for a full high-speed transfer of records.
constexpr size_t kReadBufferSize = 2048;

// Reads kept posted on the data-in endpoint. Several, as the reference driver
// keeps, so a burst of records never waits on a resubmit.
constexpr size_t kReadTransfers = 4;

// How often the reader comes out of libusb's event handling to see whether
// every posted read has come back. Not a USB timeout: the reads have none.
constexpr suseconds_t kEventPollMicros = 100000;

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

PcanDevice::PcanDevice(libusb_context* context, libusb_device_handle* handle, uint16_t productId,
                       uint8_t usbInterface, const PcanOptions& options)
    : context_(context)
    , handle_(handle)
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

    auto owned = std::shared_ptr<PcanDevice>(
        new PcanDevice(context, handle, productId, usbInterface, options));

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

    auto reading = owned->start_reader();
    if (!reading.has_value())
    {
        return std::unexpected(reading.error());
    }
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

Result<void> PcanDevice::send_commands(std::vector<uint8_t> group)
{
    auto valid = check_command_group(group);
    if (!valid.has_value())
    {
        return valid;
    }

    std::vector<uint8_t> buffer = std::move(group);
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

Result<void> PcanDevice::start_reader()
{
    readBuffers_.assign(kReadTransfers, std::vector<uint8_t>(kReadBufferSize));
    reading_ = true;
    for (std::vector<uint8_t>& buffer : readBuffers_)
    {
        libusb_transfer* const transfer = libusb_alloc_transfer(0);
        if (transfer == nullptr)
        {
            stop_reader();
            return io_error("cannot allocate a USB transfer");
        }
        readTransfers_.push_back(transfer);

        libusb_fill_bulk_transfer(transfer, handle_, firmware_.dataInEndpoint, buffer.data(),
                                  static_cast<int>(buffer.size()), &PcanDevice::on_read_complete,
                                  this, 0);
        const int rc = libusb_submit_transfer(transfer);
        if (rc != LIBUSB_SUCCESS)
        {
            stop_reader();
            return std::unexpected(from_libusb(rc, "cannot start reading from the adapter"));
        }
        ++readsInFlight_;
    }

    reader_ = std::thread([this] { handle_events_until_reads_return(); });
    return {};
}

void PcanDevice::handle_events_until_reads_return()
{
    // Until every posted read has come back -- completed for good, cancelled
    // or failed. Freeing a transfer libusb still holds is a use after free.
    while (readsInFlight_ > 0)
    {
        timeval interval { 0, kEventPollMicros };
        libusb_handle_events_timeout_completed(context_, &interval, nullptr);
    }
}

void PcanDevice::stop_reader()
{
    reading_ = false;
    for (libusb_transfer* transfer : readTransfers_)
    {
        // LIBUSB_ERROR_NOT_FOUND means it already came back; either way its
        // callback does not resubmit it now.
        libusb_cancel_transfer(transfer);
    }

    if (reader_.joinable())
    {
        reader_.join();
    }
    else
    {
        // start_reader failed part way: nothing is handling events, so the
        // cancellations are driven to completion here.
        handle_events_until_reads_return();
    }

    for (libusb_transfer* transfer : readTransfers_)
    {
        libusb_free_transfer(transfer);
    }
    readTransfers_.clear();

    // Said once, here, instead of per transfer. Zero is the normal answer and
    // says nothing; anything else is the first thing worth knowing about a
    // capture that looks wrong.
    const uint64_t reads = readFailures_.exchange(0, std::memory_order_relaxed);
    const uint64_t decodes = decodeFailures_.exchange(0, std::memory_order_relaxed);
    if (reads != 0 || decodes != 0)
    {
        SPDLOG_WARN("[pcan] {}: {} failed bulk read(s), {} undecodable transfer(s)",
                    description_, reads, decodes);
    }
}

void PcanDevice::on_read_complete(libusb_transfer* transfer)
{
    auto* const device = static_cast<PcanDevice*>(transfer->user_data);

    switch (transfer->status)
    {
    case LIBUSB_TRANSFER_COMPLETED:
        device->dispatch(std::span<const uint8_t>(
            transfer->buffer, static_cast<size_t>(std::max(0, transfer->actual_length))));
        break;

    case LIBUSB_TRANSFER_CANCELLED:
        --device->readsInFlight_;
        return;

    case LIBUSB_TRANSFER_NO_DEVICE:
        device->report_device_lost();
        --device->readsInFlight_;
        return;

    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_TIMED_OUT:
    case LIBUSB_TRANSFER_STALL:
    case LIBUSB_TRANSFER_OVERFLOW:
        // Counted, not logged: a failing link fails every transfer.
        device->readFailures_.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    if (!device->reading_)
    {
        --device->readsInFlight_;
        return;
    }

    const int rc = libusb_submit_transfer(transfer);
    if (rc != LIBUSB_SUCCESS)
    {
        // A transfer that fails with LIBUSB_TRANSFER_ERROR on an unplug (as
        // some platforms report it) is caught here, where resubmitting says
        // what the status did not.
        if (rc == LIBUSB_ERROR_NO_DEVICE)
        {
            device->report_device_lost();
        }
        else
        {
            device->readFailures_.fetch_add(1, std::memory_order_relaxed);
        }
        --device->readsInFlight_;
    }
}

void PcanDevice::report_device_lost()
{
    if (lostReported_.exchange(true))
    {
        return;
    }
    reading_ = false;

    SPDLOG_ERROR("[pcan] the adapter was unplugged");
    // Said to every open channel. Ending the reads alone left each one
    // reporting itself running while receiving nothing, forever -- a bridge
    // that looked healthy on a dead adapter.
    std::lock_guard<std::mutex> lock(sinkMutex_);
    for (RecordSink* sink : sinks_)
    {
        if (sink != nullptr)
        {
            sink->on_device_lost();
        }
    }
}

void PcanDevice::dispatch(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }

    auto decoded = decode_records(bytes);
    if (decoded.error.has_value())
    {
        // The records before the bad one are still good and are still
        // dispatched below; only the rest of this transfer is lost.
        decodeFailures_.fetch_add(1, std::memory_order_relaxed);
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

} // namespace can::pcan
