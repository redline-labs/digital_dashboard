// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_pcan/pcan_backend.h"

#include "can/dlc.h"
#include "pcan_device.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace can::pcan
{
namespace
{

// ============================================================================
// Channel
// ============================================================================

class PcanChannel
    : public Channel
    , public RecordSink
{
public:
    PcanChannel(ChannelId id, std::shared_ptr<PcanDevice> device, uint8_t localChannel,
                const OpenOptions& options)
        : id_(std::move(id))
        , device_(std::move(device))
        , localChannel_(localChannel)
        , bitrate_(options.bitrate)
        , listenOnly_(options.listenOnly)
        , queueDepth_(options.rxQueueDepth)
    {
        description_ = fmt::format("{} channel {}", device_->description(), localChannel_);
        device_->attach(localChannel_, this);
    }

    ~PcanChannel() override
    {
        (void)stop();
        device_->detach(localChannel_);
    }

    const ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }

    // --- configuration ------------------------------------------------------

    Result<void> set_bitrate(const Bitrate& bitrate) override
    {
        // The controller only accepts timing while it is in reset, so a
        // running channel is taken down and brought back up around the change
        // rather than the caller having to know that.
        const bool wasRunning = running_;
        if (wasRunning)
        {
            auto stopped = stop();
            if (!stopped.has_value())
            {
                return stopped;
            }
        }

        auto applied = apply_bitrate(bitrate);
        if (!applied.has_value())
        {
            return applied;
        }
        bitrate_ = bitrate;

        if (wasRunning)
        {
            return start();
        }
        return {};
    }

    Bitrate bitrate() const override { return bitrate_; }
    bool supports_fd() const override { return true; }

    Result<void> set_listen_only(bool listenOnly) override
    {
        listenOnly_ = listenOnly;
        if (running_)
        {
            // The mode is chosen when the controller leaves reset, so this
            // only takes effect on a restart.
            auto stopped = stop();
            if (!stopped.has_value())
            {
                return stopped;
            }
            return start();
        }
        return {};
    }

    bool listen_only() const override { return listenOnly_; }

    // --- lifecycle ----------------------------------------------------------

    Result<void> start() override
    {
        if (running_)
        {
            return {};
        }

        auto applied = apply_bitrate(bitrate_);
        if (!applied.has_value())
        {
            return applied;
        }

        std::vector<uint8_t> commands;
        // Pass everything: filtering belongs to whoever reads the topic, and a
        // filter set here would be invisible to them.
        append_std_filter_pass_all(commands, localChannel_);
        // The adapter's own timestamp-calibration traffic is of no use to a
        // bridge and would be a record per frame of overhead.
        append_options(commands, localChannel_, false, 0, kOptionCalibrationMessages);
        append_command(commands, localChannel_,
                       listenOnly_ ? Opcode::ListenOnlyMode : Opcode::NormalMode);

        auto sent = device_->send_commands(std::move(commands));
        if (!sent.has_value())
        {
            return sent;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            statistics_.state = BusState::ErrorActive;
        }
        running_ = true;
        return {};
    }

    Result<void> stop() override
    {
        if (!running_)
        {
            return {};
        }

        std::vector<uint8_t> commands;
        append_command(commands, localChannel_, Opcode::ResetMode);
        auto sent = device_->send_commands(std::move(commands));

        running_ = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            statistics_.state = BusState::Stopped;
        }
        arrived_.notify_all();

        return sent;
    }

    bool running() const override { return running_; }

    // --- traffic ------------------------------------------------------------

    Result<void> send(const helpers::CanFrame& frame) override
    {
        if (!running_)
        {
            return invalid_state(fmt::format("{} is not running", id_.toString()));
        }
        if (listenOnly_)
        {
            return invalid_state(
                fmt::format("{} is listen-only and cannot transmit", id_.toString()));
        }

        std::vector<uint8_t> buffer;
        auto encoded = append_tx_frame(buffer, localChannel_, frame, bitrate_.fd());
        if (!encoded.has_value())
        {
            return encoded;
        }

        auto sent = device_->send_frames(localChannel_, buffer);
        if (!sent.has_value())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            statistics_.txDropped++;
            return sent;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        statistics_.txFrames++;
        statistics_.txBytes += frame.len;
        return {};
    }

    Result<size_t> receive(std::span<helpers::CanFrame> out, Duration timeout) override
    {
        if (out.empty())
        {
            return size_t { 0 };
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty())
        {
            arrived_.wait_for(lock, timeout, [this] { return !queue_.empty() || !running_; });
        }

        size_t count = 0;
        while (count < out.size() && !queue_.empty())
        {
            out[count++] = queue_.front();
            queue_.pop_front();
        }
        return count;
    }

    Statistics statistics() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return statistics_;
    }

    // --- from the device's reader thread ------------------------------------

    void on_record(const Record& record) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            switch (record.type)
            {
            case RecordType::CanRx:
                if (queue_.size() >= queueDepth_)
                {
                    queue_.pop_front();
                    statistics_.rxDropped++;
                }
                queue_.push_back(record.frame);
                statistics_.rxFrames++;
                statistics_.rxBytes += record.frame.len;
                break;

            case RecordType::Status:
                if (record.state == BusState::BusOff && statistics_.state != BusState::BusOff)
                {
                    statistics_.busOffCount++;
                }
                statistics_.state = record.state;
                break;

            case RecordType::Error:
                statistics_.rxErrorCounter = record.rxErrorCounter;
                statistics_.txErrorCounter = record.txErrorCounter;
                statistics_.errorFrames++;
                break;

            case RecordType::Overrun:
                // The adapter itself dropped frames. Counting these is what
                // separates a trace with holes in it from one without.
                statistics_.rxDropped++;
                break;

            case RecordType::CanTx:
            case RecordType::BusLoad:
            case RecordType::CacheCritical:
            case RecordType::Calibration:
                break;
            }
        }

        if (record.type == RecordType::CanRx)
        {
            arrived_.notify_one();
        }
    }

private:
    Result<void> apply_bitrate(const Bitrate& bitrate)
    {
        auto nominal = solve_bit_timing(bitrate.nominalBps, bitrate.nominalSamplePointPermille,
                                        nominal_bit_timing_limits());
        if (!nominal.has_value())
        {
            return std::unexpected(nominal.error());
        }

        std::vector<uint8_t> commands;
        // Reset first: timing is only accepted while the controller is out of
        // normal mode.
        append_command(commands, localChannel_, Opcode::ResetMode);
        append_clock(commands, localChannel_, kClock80MHz);
        append_timing_slow(commands, localChannel_, *nominal);

        if (bitrate.fd())
        {
            auto data = solve_bit_timing(bitrate.dataBps, bitrate.dataSamplePointPermille,
                                         data_bit_timing_limits());
            if (!data.has_value())
            {
                return std::unexpected(data.error());
            }
            append_timing_fast(commands, localChannel_, *data);
            SPDLOG_INFO("[pcan] {}: nominal {}, data {}", id_.toString(), nominal->toString(),
                        data->toString());
        }
        else
        {
            SPDLOG_INFO("[pcan] {}: {}", id_.toString(), nominal->toString());
        }

        return device_->send_commands(std::move(commands));
    }

    ChannelId id_;
    std::string description_;
    std::shared_ptr<PcanDevice> device_;
    uint8_t localChannel_ { 0 };

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<helpers::CanFrame> queue_;

    Bitrate bitrate_;
    std::atomic<bool> listenOnly_ { false };
    std::atomic<bool> running_ { false };
    size_t queueDepth_ { 8192 };
    Statistics statistics_ {};
};

// ============================================================================
// Backend
// ============================================================================

// Where a channel lives on the hardware: which USB interface, and which of that
// interface's two channels.
//
// A PCAN-USB X6 is six CAN channels behind three USB interfaces of two, so
// channel 4 is interface 2's local channel 0. The one- and two-channel products
// are all interface 0. This mapping is the piece most in need of confirmation
// against a real X6.
struct ChannelLocation
{
    uint8_t usbInterface { 0 };
    uint8_t localChannel { 0 };
};

ChannelLocation locate(uint8_t channel)
{
    return ChannelLocation { static_cast<uint8_t>(channel / kChannelsPerInterface),
                             static_cast<uint8_t>(channel % kChannelsPerInterface) };
}

class PcanBackend : public Backend
{
public:
    explicit PcanBackend(const PcanOptions& options)
        : options_(options)
    {
        const int rc = libusb_init(&context_);
        if (rc != LIBUSB_SUCCESS)
        {
            SPDLOG_WARN("[pcan] libusb could not be initialised: {}",
                        libusb_strerror(static_cast<libusb_error>(rc)));
            context_ = nullptr;
        }
    }

    ~PcanBackend() override
    {
        // Devices hold a handle from this context, so they have to be gone
        // before it is.
        devices_.clear();
        if (context_ != nullptr)
        {
            libusb_exit(context_);
        }
    }

    const std::string& name() const override { return name_; }

    std::vector<ChannelInfo> enumerate() override
    {
        std::vector<ChannelInfo> found;
        if (context_ == nullptr)
        {
            return found;
        }

        libusb_device** list = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &list);
        if (count < 0)
        {
            SPDLOG_WARN("[pcan] cannot enumerate USB devices: {}",
                        libusb_strerror(static_cast<libusb_error>(count)));
            return found;
        }

        // Devices are indexed in discovery order, so `pcan:0` is the first
        // adapter found. A serial number is the stable way to name one, but it
        // costs a device open to read, so it is not used for the index.
        int adapterIndex = 0;
        for (ssize_t i = 0; i < count; ++i)
        {
            libusb_device_descriptor descriptor {};
            if (libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS)
            {
                continue;
            }
            if (descriptor.idVendor != kPeakVendorId)
            {
                continue;
            }

            const uint8_t channels = channel_count_for_product(descriptor.idProduct);
            if (channels == 0)
            {
                // A PEAK device this backend does not drive -- the classic
                // PCAN-USB, most likely. Worth saying so rather than being
                // silent about hardware that is plainly there.
                ChannelInfo info;
                info.id = ChannelId { "pcan", std::to_string(adapterIndex), 0 };
                info.description = fmt::format("PEAK device 0x{:04X}", descriptor.idProduct);
                info.available = false;
                info.unavailableReason =
                    "this backend drives the PCAN-USB FD family (0x0011, 0x0012, 0x0014) only";
                found.push_back(std::move(info));
                ++adapterIndex;
                continue;
            }

            for (uint8_t channel = 0; channel < channels; ++channel)
            {
                ChannelInfo info;
                info.id = ChannelId { "pcan", std::to_string(adapterIndex), channel };
                info.description = fmt::format("{} channel {}", product_name(descriptor.idProduct),
                                               channel);
                info.supportsFd = true;
                found.push_back(std::move(info));
            }
            ++adapterIndex;
        }

        libusb_free_device_list(list, 1);
        return found;
    }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id, const OpenOptions& options) override
    {
        if (context_ == nullptr)
        {
            return unsupported("libusb is not available in this build");
        }

        unsigned int adapterIndex = 0;
        const char* begin = id.device.data();
        const char* end = begin + id.device.size();
        auto [ptr, ec] = std::from_chars(begin, end, adapterIndex);
        const bool byIndex = ec == std::errc {} && ptr == end;

        libusb_device** list = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &list);
        if (count < 0)
        {
            return io_error("cannot enumerate USB devices", static_cast<int>(count));
        }

        libusb_device* match = nullptr;
        uint16_t productId = 0;
        int seen = 0;
        for (ssize_t i = 0; i < count && match == nullptr; ++i)
        {
            libusb_device_descriptor descriptor {};
            if (libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS)
            {
                continue;
            }
            if (descriptor.idVendor != kPeakVendorId
                || channel_count_for_product(descriptor.idProduct) == 0)
            {
                continue;
            }

            if (byIndex)
            {
                if (static_cast<unsigned int>(seen) == adapterIndex)
                {
                    match = list[i];
                    productId = descriptor.idProduct;
                }
                ++seen;
            }
            else
            {
                // Naming a device by serial means reading a string descriptor,
                // which needs the device open. Do it only when asked.
                libusb_device_handle* probe = nullptr;
                if (libusb_open(list[i], &probe) != LIBUSB_SUCCESS)
                {
                    continue;
                }
                unsigned char serial[64] = { 0 };
                const int length = libusb_get_string_descriptor_ascii(
                    probe, descriptor.iSerialNumber, serial, sizeof(serial) - 1);
                libusb_close(probe);
                if (length > 0
                    && id.device == reinterpret_cast<const char*>(serial))
                {
                    match = list[i];
                    productId = descriptor.idProduct;
                }
            }
        }

        if (match == nullptr)
        {
            libusb_free_device_list(list, 1);
            return not_found(fmt::format(
                "no PCAN-USB FD adapter matching '{}'. Run with --list to see what is attached",
                id.device));
        }

        const uint8_t channels = channel_count_for_product(productId);
        if (id.channel >= channels)
        {
            libusb_free_device_list(list, 1);
            return not_found(fmt::format("{} has {} channel(s); there is no channel {}",
                                         product_name(productId), channels, id.channel));
        }

        const ChannelLocation location = locate(id.channel);

        // One PcanDevice per (adapter, USB interface), shared by the channels
        // on it. This is what lets both channels of a Pro FD be open at once:
        // the second open finds the first's handle rather than failing.
        const DeviceKey key { id.device, location.usbInterface };
        std::shared_ptr<PcanDevice> device;
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            auto it = devices_.find(key);
            if (it != devices_.end())
            {
                device = it->second.lock();
            }
            if (device == nullptr)
            {
                auto opened
                    = PcanDevice::open(context_, match, productId, location.usbInterface, options_);
                if (!opened.has_value())
                {
                    libusb_free_device_list(list, 1);
                    return std::unexpected(opened.error());
                }
                device = *opened;
                devices_[key] = device;
            }
        }
        libusb_free_device_list(list, 1);

        auto channel
            = std::make_shared<PcanChannel>(id, std::move(device), location.localChannel, options);

        auto applied = channel->set_bitrate(options.bitrate);
        if (!applied.has_value())
        {
            return std::unexpected(applied.error());
        }
        if (options.start)
        {
            auto started = channel->start();
            if (!started.has_value())
            {
                return std::unexpected(started.error());
            }
        }

        return channel;
    }

private:
    using DeviceKey = std::pair<std::string, uint8_t>;

    std::string name_ { "pcan" };
    PcanOptions options_;
    libusb_context* context_ { nullptr };

    std::mutex devicesMutex_;
    // Weak, so a device closes when its last channel goes rather than living
    // as long as the backend does.
    std::map<DeviceKey, std::weak_ptr<PcanDevice>> devices_;
};

} // namespace

std::shared_ptr<Backend> make_pcan_backend(const PcanOptions& options)
{
    return std::make_shared<PcanBackend>(options);
}

} // namespace can::pcan
