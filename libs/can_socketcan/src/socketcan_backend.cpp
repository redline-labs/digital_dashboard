// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_socketcan/socketcan_backend.h"

#include "can_socketcan/netlink.h"
#include "can_socketcan/socketcan_frame.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>

#if defined(__linux__)
#include <linux/can.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <poll.h>
#endif

namespace can::socketcan
{

bool is_available()
{
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

#if defined(__linux__)

namespace
{

// The constants declared in netlink.h have to be the kernel's. Checking here
// means a kernel that renumbered something is a build failure on Linux rather
// than a message the kernel silently ignores.
static_assert(kRtmNewLink == RTM_NEWLINK, "RTM_NEWLINK has moved");
static_assert(kFlagRequest == NLM_F_REQUEST, "NLM_F_REQUEST has moved");
static_assert(kFlagAck == NLM_F_ACK, "NLM_F_ACK has moved");
static_assert(kIflaLinkInfo == IFLA_LINKINFO, "IFLA_LINKINFO has moved");
static_assert(kIflaInfoKind == IFLA_INFO_KIND, "IFLA_INFO_KIND has moved");
static_assert(kIflaInfoData == IFLA_INFO_DATA, "IFLA_INFO_DATA has moved");
static_assert(kIflaCanBittiming == IFLA_CAN_BITTIMING, "IFLA_CAN_BITTIMING has moved");
static_assert(kIflaCanCtrlMode == IFLA_CAN_CTRLMODE, "IFLA_CAN_CTRLMODE has moved");
static_assert(kIflaCanDataBittiming == IFLA_CAN_DATA_BITTIMING,
              "IFLA_CAN_DATA_BITTIMING has moved");
static_assert(kIffUp == IFF_UP, "IFF_UP has moved");
static_assert(kCanCtrlModeListenOnly == CAN_CTRLMODE_LISTENONLY, "listen-only has moved");
static_assert(kCanCtrlModeFd == CAN_CTRLMODE_FD, "CAN_CTRLMODE_FD has moved");
static_assert(kCanBittimingSize == sizeof(struct can_bittiming), "can_bittiming has changed size");
static_assert(kClassicFrameSize == sizeof(struct can_frame), "can_frame has changed size");
static_assert(kFdFrameSize == sizeof(struct canfd_frame), "canfd_frame has changed size");

Error from_errno(int code, std::string what)
{
    Error::Kind kind = Error::Kind::Io;
    switch (code)
    {
    case EACCES:
    case EPERM:
        kind = Error::Kind::PermissionDenied;
        break;
    case ENODEV:
    case ENXIO:
    case ENOENT:
        kind = Error::Kind::NotFound;
        break;
    case EBUSY:
        kind = Error::Kind::Busy;
        break;
    case EOPNOTSUPP:
        kind = Error::Kind::Unsupported;
        break;
    default:
        break;
    }
    return Error { kind, fmt::format("{}: {}", what, std::strerror(code)), code };
}

// One netlink round trip. Opening a socket per request rather than holding one
// open costs a syscall on a path that runs when a bit rate changes, which is
// rare, and avoids a long-lived privileged socket.
Result<void> send_link_request(const LinkRequest& request)
{
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        return std::unexpected(from_errno(errno, "cannot open a netlink socket"));
    }

    struct FdGuard
    {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard { fd };

    sockaddr_nl address {};
    address.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        return std::unexpected(from_errno(errno, "cannot bind the netlink socket"));
    }

    static std::atomic<uint32_t> nextSequence { 1 };
    const uint32_t sequence = nextSequence++;

    auto message = encode_link_request(request, sequence);
    if (!message.has_value())
    {
        return std::unexpected(message.error());
    }

    sockaddr_nl kernel {};
    kernel.nl_family = AF_NETLINK;

    const ssize_t sent = ::sendto(fd, message->data(), message->size(), 0,
                                  reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel));
    if (sent < 0)
    {
        return std::unexpected(from_errno(errno, "cannot send the netlink request"));
    }

    std::array<uint8_t, 4096> reply {};
    const ssize_t received = ::recv(fd, reply.data(), reply.size(), 0);
    if (received < 0)
    {
        return std::unexpected(from_errno(errno, "no answer to the netlink request"));
    }

    auto ack = decode_ack(std::span(reply.data(), static_cast<size_t>(received)));
    if (!ack.has_value())
    {
        return std::unexpected(ack.error());
    }
    if (ack->error != 0)
    {
        auto error = from_errno(ack->error,
                                fmt::format("the kernel refused the change to {}",
                                            request.interface));
        if (error.kind == Error::Kind::PermissionDenied)
        {
            error.message += " -- changing a CAN interface's bit rate needs CAP_NET_ADMIN, the "
                             "same as 'ip link set'";
        }
        return std::unexpected(error);
    }

    return {};
}

// ============================================================================
// Channel
// ============================================================================

class SocketCanChannel : public Channel
{
public:
    SocketCanChannel(ChannelId id, int fd, const OpenOptions& options)
        : id_(std::move(id))
        , description_(fmt::format("SocketCAN interface {}", id_.device))
        , fd_(fd)
        , bitrate_(options.bitrate)
        , listenOnly_(options.listenOnly)
    {
    }

    ~SocketCanChannel() override
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    const ChannelId& id() const override { return id_; }
    const std::string& description() const override { return description_; }

    Result<void> set_bitrate(const Bitrate& bitrate) override
    {
        // The kernel will not retime a running interface, so it goes down and
        // comes back up around the change -- the same sequence `ip` performs.
        auto nominal = solve_bit_timing(bitrate.nominalBps, bitrate.nominalSamplePointPermille,
                                        limits());
        if (!nominal.has_value())
        {
            return std::unexpected(nominal.error());
        }

        std::optional<BitTiming> data;
        if (bitrate.fd())
        {
            auto solved = solve_bit_timing(bitrate.dataBps, bitrate.dataSamplePointPermille,
                                           dataLimits());
            if (!solved.has_value())
            {
                return std::unexpected(solved.error());
            }
            data = *solved;
        }

        const bool wasUp = running_;
        if (wasUp)
        {
            LinkRequest down;
            down.interface = id_.device;
            down.up = false;
            auto result = send_link_request(down);
            if (!result.has_value())
            {
                return result;
            }
        }

        LinkRequest configure;
        configure.interface = id_.device;
        configure.nominal = *nominal;
        configure.data = data;
        configure.fd = bitrate.fd();
        configure.listenOnly = listenOnly_;
        auto configured = send_link_request(configure);
        if (!configured.has_value())
        {
            return configured;
        }

        bitrate_ = bitrate;

        if (wasUp)
        {
            LinkRequest up;
            up.interface = id_.device;
            up.up = true;
            return send_link_request(up);
        }
        return {};
    }

    Bitrate bitrate() const override { return bitrate_; }

    bool supports_fd() const override { return fdFrames_; }

    Result<void> set_listen_only(bool listenOnly) override
    {
        listenOnly_ = listenOnly;
        return set_bitrate(bitrate_);
    }

    bool listen_only() const override { return listenOnly_; }

    Result<void> start() override
    {
        LinkRequest up;
        up.interface = id_.device;
        up.up = true;
        auto result = send_link_request(up);
        if (result.has_value())
        {
            running_ = true;
        }
        return result;
    }

    Result<void> stop() override
    {
        LinkRequest down;
        down.interface = id_.device;
        down.up = false;
        auto result = send_link_request(down);
        running_ = false;
        return result;
    }

    bool running() const override { return running_; }

    Result<void> send(const helpers::CanFrame& frame) override
    {
        std::array<uint8_t, kFdFrameSize> buffer {};
        auto size = encode_frame(frame, buffer);
        if (!size.has_value())
        {
            return std::unexpected(size.error());
        }

        // write() on a CAN_RAW socket is atomic per frame, so no lock is
        // needed for two threads to share it -- which is what makes send()
        // callable while another thread is blocked in receive().
        const ssize_t written = ::write(fd_, buffer.data(), *size);
        if (written < 0)
        {
            statistics_.txDropped++;
            return std::unexpected(from_errno(errno, "cannot transmit"));
        }

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
            return std::unexpected(from_errno(errno, "cannot wait for frames"));
        }
        if (ready == 0)
        {
            // A quiet bus, not a failure.
            return size_t { 0 };
        }

        size_t count = 0;
        while (count < out.size())
        {
            std::array<uint8_t, kFdFrameSize> buffer {};
            std::array<uint8_t, 64> control {};

            iovec iov {};
            iov.iov_base = buffer.data();
            iov.iov_len = buffer.size();

            msghdr message {};
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();

            const ssize_t got = ::recvmsg(fd_, &message, MSG_DONTWAIT);
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
                return std::unexpected(from_errno(errno, "cannot receive"));
            }

            auto frame = decode_frame(std::span(buffer.data(), static_cast<size_t>(got)));
            if (!frame.has_value())
            {
                SPDLOG_WARN("[socketcan] {}", to_string(frame.error()));
                continue;
            }

            // The kernel timestamps on arrival, which is closer to the wire
            // than anything measured after the read returns.
            for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
                 header = CMSG_NXTHDR(&message, header))
            {
                if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SO_TIMESTAMP)
                {
                    timeval stamp {};
                    std::memcpy(&stamp, CMSG_DATA(header), sizeof(stamp));
                    frame->timestampUs = static_cast<uint64_t>(stamp.tv_sec) * 1000000ull
                        + static_cast<uint64_t>(stamp.tv_usec);
                }
            }

            if (frame->isError)
            {
                statistics_.errorFrames++;
            }
            statistics_.rxFrames++;
            statistics_.rxBytes += frame->len;
            out[count++] = *frame;
        }

        return count;
    }

    Statistics statistics() const override
    {
        Statistics copy = statistics_;
        copy.state = running_ ? BusState::ErrorActive : BusState::Stopped;
        return copy;
    }

    void set_fd_frames(bool enabled) { fdFrames_ = enabled; }
    void set_running(bool running) { running_ = running; }

private:
    // The kernel knows the controller's real clock and limits; this library
    // does not have a way to ask for them per interface, so it solves against
    // a permissive set and lets the kernel refuse anything its controller
    // cannot do. The bit rate that comes back in the reply is authoritative.
    static BitTimingLimits limits()
    {
        BitTimingLimits l;
        l.clockHz = 80000000;
        l.tseg1Min = 1;
        l.tseg1Max = 64;
        l.tseg2Min = 1;
        l.tseg2Max = 16;
        l.sjwMax = 16;
        l.brpMin = 1;
        l.brpMax = 1024;
        return l;
    }

    static BitTimingLimits dataLimits()
    {
        BitTimingLimits l = limits();
        l.tseg1Max = 16;
        l.tseg2Max = 8;
        l.sjwMax = 4;
        return l;
    }

    ChannelId id_;
    std::string description_;
    int fd_ { -1 };
    Bitrate bitrate_;
    std::atomic<bool> listenOnly_ { false };
    std::atomic<bool> running_ { false };
    bool fdFrames_ { false };
    Statistics statistics_ {};
};

// ============================================================================
// Backend
// ============================================================================

class LinuxSocketCanBackend : public Backend
{
public:
    const std::string& name() const override { return name_; }

    std::vector<ChannelInfo> enumerate() override
    {
        std::vector<ChannelInfo> found;

        // /sys/class/net/<name>/type holds the ARP hardware type; 280 is
        // ARPHRD_CAN. Reading sysfs rather than dumping links over netlink
        // keeps enumeration to a directory walk.
        constexpr int kArphrdCan = 280;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", ec))
        {
            std::ifstream type(entry.path() / "type");
            int value = 0;
            if (!(type >> value) || value != kArphrdCan)
            {
                continue;
            }

            ChannelInfo info;
            info.id = ChannelId { "socketcan", entry.path().filename().string(), 0 };
            info.description = fmt::format("SocketCAN interface {}", info.id.device);
            // Whether the controller does FD is in
            // /sys/class/net/<name>/../ but not consistently, so it is left
            // for the open to discover rather than guessed at here.
            info.supportsFd = true;
            found.push_back(std::move(info));
        }

        if (ec)
        {
            SPDLOG_DEBUG("[socketcan] cannot read /sys/class/net: {}", ec.message());
        }

        return found;
    }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id, const OpenOptions& options) override
    {
        if (id.channel != 0)
        {
            return invalid_argument(fmt::format(
                "'{}' has a channel suffix; a SocketCAN interface is one channel, so use "
                "'socketcan:{}'",
                id.toString(), id.device));
        }
        if (!is_valid_interface_name(id.device))
        {
            return invalid_argument(fmt::format("'{}' is not a valid interface name", id.device));
        }

        const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
        if (fd < 0)
        {
            return std::unexpected(from_errno(errno, "cannot open a CAN_RAW socket"));
        }

        ifreq request {};
        std::strncpy(request.ifr_name, id.device.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFINDEX, &request) < 0)
        {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(
                from_errno(saved, fmt::format("no interface named '{}'", id.device)));
        }

        sockaddr_can address {};
        address.can_family = AF_CAN;
        address.can_ifindex = request.ifr_ifindex;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(
                from_errno(saved, fmt::format("cannot bind to '{}'", id.device)));
        }

        auto channel = std::make_shared<SocketCanChannel>(id, fd, options);

        // Asking for FD frames changes what a read returns, so it has to be
        // set before any traffic. A controller that cannot do FD refuses here,
        // which is a clearer answer than an FD frame failing to transmit later.
        if (options.bitrate.fd())
        {
            const int enable = 1;
            if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0)
            {
                return std::unexpected(from_errno(
                    errno, fmt::format("'{}' cannot carry CAN FD frames", id.device)));
            }
            channel->set_fd_frames(true);
        }

        // Kernel arrival timestamps. Best-effort: losing them costs accuracy
        // in the log, not correctness.
        const int stamp = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &stamp, sizeof(stamp)) < 0)
        {
            SPDLOG_DEBUG("[socketcan] {} will not timestamp frames: {}", id.device,
                         std::strerror(errno));
        }

        // Configuring the link needs CAP_NET_ADMIN. A caller that only wants
        // to read and write an interface someone else already configured
        // should not be forced to have it, so a refusal here is reported and
        // the socket is kept.
        auto configured = channel->set_bitrate(options.bitrate);
        if (!configured.has_value())
        {
            if (configured.error().kind == Error::Kind::PermissionDenied)
            {
                SPDLOG_WARN("[socketcan] cannot set {}'s bit rate: {}", id.device,
                            configured.error().message);
                SPDLOG_WARN("[socketcan] carrying on with whatever it is already configured for");
            }
            else
            {
                return std::unexpected(configured.error());
            }
        }

        if (options.start)
        {
            auto started = channel->start();
            if (!started.has_value())
            {
                if (started.error().kind == Error::Kind::PermissionDenied)
                {
                    SPDLOG_WARN("[socketcan] cannot bring {} up: {}", id.device,
                                started.error().message);
                    // The interface may well already be up, in which case
                    // reading and writing works regardless.
                    channel->set_running(true);
                }
                else
                {
                    return std::unexpected(started.error());
                }
            }
        }

        return channel;
    }

private:
    std::string name_ { "socketcan" };
};

} // namespace

std::shared_ptr<Backend> make_socketcan_backend()
{
    return std::make_shared<LinuxSocketCanBackend>();
}

#else // not Linux

namespace
{

// Present but empty, rather than absent. A node's channel list then means the
// same thing on every platform: `socketcan:can0` fails with a reason rather
// than with "no backend named socketcan", which would suggest a build problem
// rather than an operating system.
class UnavailableSocketCanBackend : public Backend
{
public:
    const std::string& name() const override { return name_; }

    std::vector<ChannelInfo> enumerate() override { return {}; }

    Result<std::shared_ptr<Channel>> open(const ChannelId& id, const OpenOptions&) override
    {
        return unsupported(fmt::format(
            "SocketCAN is a Linux kernel interface and this is not Linux, so '{}' cannot be "
            "opened. Use the pcan backend for a PCAN adapter over libusb, or 'virtual:<name>' "
            "for a loopback bus",
            id.toString()));
    }

private:
    std::string name_ { "socketcan" };
};

} // namespace

std::shared_ptr<Backend> make_socketcan_backend()
{
    return std::make_shared<UnavailableSocketCanBackend>();
}

#endif

} // namespace can::socketcan
