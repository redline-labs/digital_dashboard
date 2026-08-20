// SPDX-License-Identifier: GPL-3.0-or-later

#include "utc_transport.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace can::motec
{

Transport::~Transport() = default;

namespace
{

Error from_libusb(int code, std::string what)
{
    Error::Kind kind = Error::Kind::Io;
    switch (code)
    {
    case LIBUSB_ERROR_ACCESS:
        kind = Error::Kind::PermissionDenied;
        break;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        kind = Error::Kind::NotFound;
        break;
    case LIBUSB_ERROR_BUSY:
        kind = Error::Kind::Busy;
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

Error from_errno(int code, std::string what)
{
    Error::Kind kind = Error::Kind::Io;
    switch (code)
    {
    case EACCES:
    case EPERM:
        kind = Error::Kind::PermissionDenied;
        break;
    case ENOENT:
    case EHOSTUNREACH:
    case ENETUNREACH:
        kind = Error::Kind::NotFound;
        break;
    default:
        break;
    }
    return Error { kind, fmt::format("{}: {}", what, std::strerror(code)), code };
}

// A libusb context shared by every USB transport in the process. One context
// is enough and creating one per device would multiply the event-handling
// threads libusb runs behind the scenes.
libusb_context* shared_context()
{
    static libusb_context* context = []() -> libusb_context* {
        libusb_context* created = nullptr;
        const int rc = libusb_init(&created);
        if (rc != LIBUSB_SUCCESS)
        {
            SPDLOG_WARN("[motec] libusb could not be initialised: {}",
                        libusb_strerror(static_cast<libusb_error>(rc)));
            return nullptr;
        }
        return created;
    }();
    return context;
}

std::string read_serial(libusb_device_handle* handle, const libusb_device_descriptor& descriptor)
{
    if (descriptor.iSerialNumber == 0)
    {
        return {};
    }
    std::array<unsigned char, 64> text {};
    const int length = libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber,
                                                          text.data(),
                                                          static_cast<int>(text.size()) - 1);
    if (length <= 0)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text.data()), static_cast<size_t>(length));
}

// ---------------------------------------------------------------------------
// USB
// ---------------------------------------------------------------------------

class UsbTransport : public Transport
{
public:
    UsbTransport(libusb_device_handle* handle, std::string description, unsigned int readTimeoutMs,
                 unsigned int writeTimeoutMs, bool detachedKernelDriver)
        : handle_(handle)
        , description_(std::move(description))
        , readTimeoutMs_(readTimeoutMs)
        , writeTimeoutMs_(writeTimeoutMs)
        , detachedKernelDriver_(detachedKernelDriver)
    {
    }

    ~UsbTransport() override
    {
        if (handle_ == nullptr)
        {
            return;
        }
        libusb_release_interface(handle_, kInterface);
        if (detachedKernelDriver_)
        {
            libusb_attach_kernel_driver(handle_, kInterface);
        }
        libusb_close(handle_);
    }

    const std::string& description() const override { return description_; }

    Result<void> send(std::span<const uint8_t> bytes) override
    {
        // libusb permits concurrent transfers, but two writers interleaving on
        // one bulk endpoint would split a frame across another's, and the
        // device has no way to tell. One frame per write, under a lock.
        std::lock_guard<std::mutex> lock(writeMutex_);

        int transferred = 0;
        const int rc = libusb_bulk_transfer(
            handle_, kBulkOutEndpoint, const_cast<unsigned char*>(bytes.data()),
            static_cast<int>(bytes.size()), &transferred, writeTimeoutMs_);
        if (rc != LIBUSB_SUCCESS)
        {
            return std::unexpected(from_libusb(rc, "cannot write to the UTC"));
        }
        if (static_cast<size_t>(transferred) != bytes.size())
        {
            return io_error(fmt::format("wrote {} of {} bytes to the UTC", transferred,
                                        bytes.size()));
        }
        return {};
    }

    Result<size_t> receive(std::vector<Frame>& out, Duration timeout) override
    {
        // Anything the last read left half-parsed comes out first, before
        // going back to the device: a single transfer routinely carries
        // several frames.
        const size_t already = drain(out);
        if (already != 0)
        {
            return already;
        }

        // A read big enough for several packets. The FT245 hands over whatever
        // it has, so a larger buffer means fewer round trips on a busy bus and
        // costs nothing on an idle one.
        std::array<uint8_t, 16 * kUsbPacketSize> buffer {};
        int transferred = 0;
        const unsigned int waitMs = std::min<unsigned int>(
            readTimeoutMs_, static_cast<unsigned int>(std::max<int64_t>(1, timeout.count())));

        const int rc = libusb_bulk_transfer(handle_, kBulkInEndpoint, buffer.data(),
                                            static_cast<int>(buffer.size()), &transferred, waitMs);
        if (rc == LIBUSB_ERROR_TIMEOUT)
        {
            // Not a failure. An FT245 with nothing to say answers with its two
            // status bytes or with nothing at all.
            return size_t { 0 };
        }
        if (rc != LIBUSB_SUCCESS)
        {
            return std::unexpected(from_libusb(rc, "cannot read from the UTC"));
        }

        reader_.push(strip_ftdi_status(std::span(buffer.data(), static_cast<size_t>(transferred))));
        return drain(out);
    }

    uint64_t resyncBytes() const override { return reader_.resyncBytes(); }

    static constexpr int kInterface = 0;

private:
    size_t drain(std::vector<Frame>& out)
    {
        size_t count = 0;
        while (auto frame = reader_.next())
        {
            out.push_back(std::move(*frame));
            ++count;
        }
        return count;
    }

    libusb_device_handle* handle_ { nullptr };
    std::string description_;
    unsigned int readTimeoutMs_ { 100 };
    unsigned int writeTimeoutMs_ { 1000 };
    bool detachedKernelDriver_ { false };

    // Only touched from the receive thread; the Channel contract says one
    // thread receives.
    FrameReader reader_;
    std::mutex writeMutex_;
};

} // namespace

std::vector<UsbDeviceInfo> list_usb_devices()
{
    std::vector<UsbDeviceInfo> found;

    libusb_context* context = shared_context();
    if (context == nullptr)
    {
        return found;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0)
    {
        SPDLOG_WARN("[motec] cannot enumerate USB devices: {}",
                    libusb_strerror(static_cast<libusb_error>(count)));
        return found;
    }

    unsigned int index = 0;
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS)
        {
            continue;
        }
        if (descriptor.idVendor != kFtdiVendorId || descriptor.idProduct != kUtcProductId)
        {
            continue;
        }

        UsbDeviceInfo info;
        info.index = index++;

        // Opening is what answers "could this actually be used", and it is the
        // only way to read the serial. Both are worth the open on a path that
        // runs when a human asks what is attached.
        libusb_device_handle* handle = nullptr;
        const int rc = libusb_open(list[i], &handle);
        if (rc != LIBUSB_SUCCESS)
        {
            info.available = false;
            info.unavailableReason = libusb_strerror(static_cast<libusb_error>(rc));
            if (rc == LIBUSB_ERROR_ACCESS)
            {
                info.unavailableReason +=
                    fmt::format(" -- a udev rule for {:04x}:{:04x} is missing", kFtdiVendorId,
                                kUtcProductId);
            }
            found.push_back(std::move(info));
            continue;
        }

        info.serial = read_serial(handle, descriptor);
#if defined(__linux__)
        if (libusb_kernel_driver_active(handle, UsbTransport::kInterface) == 1)
        {
            // Nothing in mainline binds to this product id, so this means
            // ftdi_sio was told to with a new_id write. Say which driver it is
            // rather than leaving a bare EBUSY.
            info.available = false;
            info.unavailableReason =
                "a kernel driver (probably ftdi_sio, via a new_id write) holds this dongle";
        }
#endif
        libusb_close(handle);
        found.push_back(std::move(info));
    }

    libusb_free_device_list(list, 1);
    return found;
}

Result<std::shared_ptr<Transport>> open_usb_transport(const UsbTarget& target,
                                                      unsigned int readTimeoutMs,
                                                      unsigned int writeTimeoutMs,
                                                      bool detachKernelDriver)
{
    libusb_context* context = shared_context();
    if (context == nullptr)
    {
        return unsupported("libusb is not available in this build");
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0)
    {
        return io_error("cannot enumerate USB devices", static_cast<int>(count));
    }

    struct ListGuard
    {
        libusb_device** list;
        ~ListGuard() { libusb_free_device_list(list, 1); }
    } guard { list };

    libusb_device* match = nullptr;
    std::string matchedSerial;
    // A serial can only be read from an open device, so a dongle this process
    // may not open is a dongle whose serial cannot be compared. Without this,
    // "permission denied" presents as "no such device" -- which sends whoever
    // hits it looking for a cable fault.
    bool sawUnopenable = false;

    // Every attached UTC, in discovery order, so the index pass and the serial
    // pass walk the same list.
    std::vector<libusb_device*> dongles;
    for (ssize_t i = 0; i < count; ++i)
    {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(list[i], &descriptor) != LIBUSB_SUCCESS)
        {
            continue;
        }
        if (descriptor.idVendor == kFtdiVendorId && descriptor.idProduct == kUtcProductId)
        {
            dongles.push_back(list[i]);
        }
    }

    if (target.byIndex && target.index < dongles.size())
    {
        match = dongles[target.index];
    }

    // Falls through to the serial when the index did not resolve, which is
    // what makes `motec:56536` find the dongle with that serial rather than
    // insisting on a fifty-six-thousandth adapter.
    if (match == nullptr && !target.serial.empty())
    {
        for (libusb_device* device : dongles)
        {
            libusb_device_descriptor descriptor {};
            if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS)
            {
                continue;
            }

            // Matching by serial costs an open, so it is only done once the
            // index has failed to answer.
            libusb_device_handle* probe = nullptr;
            if (libusb_open(device, &probe) != LIBUSB_SUCCESS)
            {
                sawUnopenable = true;
                continue;
            }
            const std::string serial = read_serial(probe, descriptor);
            libusb_close(probe);
            if (serial == target.serial)
            {
                match = device;
                matchedSerial = serial;
                break;
            }
        }
    }

    if (match == nullptr)
    {
        const std::string wanted = target.serial.empty() ? std::to_string(target.index)
                                                         : target.serial;
        if (sawUnopenable)
        {
            return permission_denied(fmt::format(
                "a MoTeC UTC is attached but its serial could not be read to compare against "
                "'{}', because opening it was denied -- on Linux this usually means a udev rule "
                "is missing for {:04x}:{:04x}",
                wanted, kFtdiVendorId, kUtcProductId));
        }
        if (dongles.empty())
        {
            return not_found("no MoTeC UTC is attached. Run with --list to see what is");
        }
        return not_found(fmt::format(
            "no MoTeC UTC matches '{}'; {} attached. Run with --list to see their serials",
            wanted, dongles.size()));
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(match, &handle);
    if (rc != LIBUSB_SUCCESS)
    {
        auto error = from_libusb(rc, "cannot open the MoTeC UTC");
        if (error.kind == Error::Kind::PermissionDenied)
        {
            error.message += fmt::format(
                " -- on Linux this usually means a udev rule is missing for {:04x}:{:04x}",
                kFtdiVendorId, kUtcProductId);
        }
        return std::unexpected(error);
    }

    bool detached = false;
#if defined(__linux__)
    if (libusb_kernel_driver_active(handle, UsbTransport::kInterface) == 1)
    {
        if (!detachKernelDriver)
        {
            libusb_close(handle);
            return busy("a kernel driver holds this dongle. Nothing in mainline Linux binds to "
                        "MoTeC's product id, so this is ftdi_sio after a new_id write -- unbind "
                        "it, or pass the option to detach the kernel driver");
        }
        rc = libusb_detach_kernel_driver(handle, UsbTransport::kInterface);
        if (rc != LIBUSB_SUCCESS)
        {
            libusb_close(handle);
            return std::unexpected(from_libusb(rc, "cannot detach the kernel driver"));
        }
        detached = true;
    }
#else
    (void)detachKernelDriver;
#endif

    rc = libusb_claim_interface(handle, UsbTransport::kInterface);
    if (rc != LIBUSB_SUCCESS)
    {
        auto error = from_libusb(rc, "cannot claim the UTC's interface");
        if (error.kind == Error::Kind::Busy)
        {
            error.message += " -- something else has this dongle open";
        }
        libusb_close(handle);
        return std::unexpected(error);
    }

    if (matchedSerial.empty())
    {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(match, &descriptor) == LIBUSB_SUCCESS)
        {
            matchedSerial = read_serial(handle, descriptor);
        }
    }

    // Deliberately no SET_BAUD_RATE, SET_LATENCY_TIMER or SET_FLOW_CTRL. The
    // FT245BM is a FIFO part with none of those; issuing them is what a driver
    // written against the far more common FT232 would do first, and the device
    // does not answer them.

    const std::string description = matchedSerial.empty()
        ? std::string("MoTeC UTC")
        : fmt::format("MoTeC UTC serial {}", matchedSerial);

    return std::static_pointer_cast<Transport>(std::make_shared<UsbTransport>(
        handle, description, readTimeoutMs, writeTimeoutMs, detached));
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------

namespace
{

class UdpTransport : public Transport
{
public:
    UdpTransport(int fd, std::string description)
        : fd_(fd)
        , description_(std::move(description))
    {
    }

    ~UdpTransport() override
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    const std::string& description() const override { return description_; }

    Result<void> send(std::span<const uint8_t> bytes) override
    {
        // One frame per datagram, which is the whole framing rule on this
        // transport. No lock: a single send() on a connected UDP socket is
        // atomic, so two threads cannot interleave halves of a frame.
        const ssize_t sent = ::send(fd_, bytes.data(), bytes.size(), 0);
        if (sent < 0)
        {
            return std::unexpected(from_errno(errno, "cannot send to the gateway"));
        }
        return {};
    }

    Result<size_t> receive(std::vector<Frame>& out, Duration timeout) override
    {
        pollfd poller {};
        poller.fd = fd_;
        poller.events = POLLIN;

        const int ready = ::poll(&poller, 1, static_cast<int>(timeout.count()));
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                return size_t { 0 };
            }
            return std::unexpected(from_errno(errno, "cannot wait for gateway datagrams"));
        }
        if (ready == 0)
        {
            return size_t { 0 };
        }

        size_t count = 0;
        for (;;)
        {
            std::array<uint8_t, 2048> buffer {};
            const ssize_t got = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (got < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                if (errno == EINTR)
                {
                    continue;
                }
                return std::unexpected(from_errno(errno, "cannot receive from the gateway"));
            }

            auto frame = decode_frame(std::span(buffer.data(), static_cast<size_t>(got)));
            if (!frame.has_value())
            {
                // A datagram is all-or-nothing, so a bad one is dropped and
                // counted rather than resynchronised past.
                SPDLOG_WARN("[motec] dropping a malformed {}-byte datagram: {}", got,
                            to_string(frame.error()));
                continue;
            }
            out.push_back(std::move(*frame));
            ++count;
        }
        return count;
    }

private:
    int fd_ { -1 };
    std::string description_;
};

} // namespace

Result<std::shared_ptr<Transport>> open_udp_transport(const std::string& host, uint16_t port)
{
    const uint16_t wanted = port == 0 ? kDefaultGatewayPort : port;

    addrinfo hints {};
    // AF_UNSPEC rather than AF_INET6: MoTeC's own gateways are IPv6, but the
    // simulator this path was developed against is reachable over either, and
    // refusing a v4 address would be a restriction with nothing behind it.
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* resolved = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(wanted).c_str(), &hints, &resolved);
    if (rc != 0)
    {
        return not_found(fmt::format("cannot resolve '{}': {}", host, ::gai_strerror(rc)));
    }

    struct AddrGuard
    {
        addrinfo* list;
        ~AddrGuard() { ::freeaddrinfo(list); }
    } guard { resolved };

    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        const int fd = ::socket(candidate->ai_family, candidate->ai_socktype | SOCK_CLOEXEC,
                                candidate->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        // Connected, so the gateway's replies are filtered to the address we
        // asked -- and so send() needs no destination.
        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) < 0)
        {
            ::close(fd);
            continue;
        }
        return std::static_pointer_cast<Transport>(std::make_shared<UdpTransport>(
            fd, fmt::format("MoTeC gateway at {}:{}", host, wanted)));
    }

    return io_error(fmt::format("cannot reach a MoTeC gateway at {}:{}", host, wanted));
}

} // namespace can::motec
