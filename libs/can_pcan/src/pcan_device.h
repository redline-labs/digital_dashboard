// SPDX-License-Identifier: GPL-3.0-or-later
//
// One PCAN USB interface: the libusb handle, the reader thread, and the
// demultiplexing that turns one stream of records into per-channel traffic.
//
// Internal to the backend. Nothing outside libs/can_pcan should include this --
// the public surface is a can::Channel obtained from the backend.
#ifndef CAN_PCAN_DEVICE_H
#define CAN_PCAN_DEVICE_H

#include "can_pcan/pcan_backend.h"
#include "can_pcan/pucan.h"

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace can::pcan
{

// A PCAN USB interface carries at most this many CAN channels. The Pro FD uses
// both, the USB FD one; the X6's six channels are three interfaces of two.
inline constexpr uint8_t kChannelsPerInterface = 2;

// Where decoded records go. Implemented by PcanChannel.
class RecordSink
{
public:
    virtual ~RecordSink() = default;
    virtual void on_record(const Record& record) = 0;
};

class PcanDevice
{
public:
    // `usbInterface` is the USB interface number, which for the X6 is what
    // selects a pair of CAN channels.
    static Result<std::shared_ptr<PcanDevice>> open(libusb_context* context, libusb_device* device,
                                                    uint16_t productId, uint8_t usbInterface,
                                                    const PcanOptions& options);

    ~PcanDevice();

    PcanDevice(const PcanDevice&) = delete;
    PcanDevice& operator=(const PcanDevice&) = delete;

    const FirmwareInfo& firmware() const { return firmware_; }
    uint16_t product_id() const { return productId_; }
    uint8_t usb_interface() const { return usbInterface_; }
    const std::string& description() const { return description_; }

    // Local channel index within this interface, 0 or 1.
    void attach(uint8_t localChannel, RecordSink* sink);
    void detach(uint8_t localChannel);

    // Pushes a built command buffer to the command endpoint.
    Result<void> send_commands(std::vector<uint8_t> buffer);

    // Pushes encoded frames to the data endpoint for a channel.
    Result<void> send_frames(uint8_t localChannel, std::span<const uint8_t> buffer);

private:
    PcanDevice(libusb_device_handle* handle, uint16_t productId, uint8_t usbInterface,
               const PcanOptions& options);

    void start_reader();
    void stop_reader();
    void reader_loop();

    libusb_device_handle* handle_ { nullptr };
    uint16_t productId_ { 0 };
    uint8_t usbInterface_ { 0 };
    PcanOptions options_;
    FirmwareInfo firmware_;
    std::string description_;
    // Only ever set on Linux, where the in-tree driver has to be displaced.
    [[maybe_unused]] bool detachedKernelDriver_ { false };

    // libusb serialises transfers on a handle internally, but two threads
    // building a command buffer into the same device would still interleave,
    // so command submission is serialised here.
    std::mutex writeMutex_;

    std::mutex sinkMutex_;
    std::array<RecordSink*, kChannelsPerInterface> sinks_ { nullptr, nullptr };

    std::thread reader_;
    std::atomic<bool> reading_ { false };

    // Counted, not logged. Both of these sit on the per-transfer read path, and
    // a link that is failing fails every transfer -- which turned a broken
    // adapter into thousands of identical lines a second, each one costing more
    // than the read that produced it. Reported once, when the reader stops.
    std::atomic<uint64_t> readFailures_ { 0 };
    std::atomic<uint64_t> decodeFailures_ { 0 };
};

} // namespace can::pcan

#endif // CAN_PCAN_DEVICE_H
