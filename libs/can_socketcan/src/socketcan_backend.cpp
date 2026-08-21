// SPDX-License-Identifier: GPL-3.0-or-later

#include "can_socketcan/socketcan_backend.h"

#include "can_socketcan/netlink.h"
#include "can_socketcan/socketcan_frame.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>

#if defined(__linux__)
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <linux/capability.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
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

// The frame sizes in socketcan_frame.h are hand-written, because the parsing
// there is: a read returns a struct and its size is the only thing that says
// which of the two it is. Checking against the kernel's own structures means a
// layout change is a build failure rather than a misparsed frame.
//
// The netlink constants used to be checked here in the same way. They are taken
// from the kernel's headers directly now, so there is nothing left to compare.
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

// Whether this process could configure an interface if it wanted to. Reading
// your own capabilities needs none of them, so this is safe to ask anywhere,
// and it is what separates "this interface is down and you cannot fix it" from
// "this interface is down and open() will bring it up".
//
// Not the same question as geteuid() == 0: a binary given the capability with
// `setcap` has it without being root, and a root process in a container may
// have had it dropped.
bool have_net_admin()
{
    __user_cap_header_struct header {};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;

    // Two words since version 3; CAP_NET_ADMIN is 12, so it lives in the first.
    std::array<__user_cap_data_struct, 2> data {};
    if (::syscall(SYS_capget, &header, data.data()) != 0)
    {
        return false;
    }
    return (data[CAP_NET_ADMIN >> 5].effective & (1u << (CAP_NET_ADMIN & 31))) != 0;
}

// The read counterpart of send_link_request. Unprivileged, which is the whole
// point: when CAP_NET_ADMIN is missing every write above fails and this is the
// only thing left that can say what the interface really is.
Result<LinkState> query_link(const std::string& interface)
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
    auto message = encode_link_query(interface, nextSequence++);
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
        return std::unexpected(from_errno(errno, "cannot send a netlink link query"));
    }

    // A link reply carries every attribute the kernel has for the interface,
    // which for a CAN device with its timing tables runs to a couple of
    // kilobytes. Undersizing this truncates the answer rather than failing it.
    std::array<uint8_t, 8192> reply {};
    const ssize_t got = ::recv(fd, reply.data(), reply.size(), 0);
    if (got < 0)
    {
        return std::unexpected(from_errno(errno, "cannot read the netlink reply"));
    }

    return decode_link_state(std::span(reply.data(), static_cast<size_t>(got)));
}

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

        // The interface's actual state, not what this object last asked for:
        // something else may have brought it up, and the kernel refuses to
        // retime a running interface with EBUSY rather than by explaining.
        auto observed = link_state();
        const bool wasUp = observed.has_value() ? observed->up : running_.load();
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
        invalidate_state();

        if (wasUp)
        {
            LinkRequest up;
            up.interface = id_.device;
            up.up = true;
            auto restored = send_link_request(up);
            invalidate_state();
            return restored;
        }
        return {};
    }

    // What the kernel says, falling back to what was asked for only when it
    // has nothing to say -- an interface that has never been given a bit rate
    // carries no bittiming attribute at all.
    Bitrate bitrate() const override
    {
        auto state = link_state();
        if (!state.has_value())
        {
            return bitrate_;
        }

        Bitrate actual = bitrate_;
        if (state->nominalBps.has_value())
        {
            actual.nominalBps = *state->nominalBps;
            actual.nominalSamplePointPermille = state->nominalSamplePointPermille.value_or(0);
        }
        // Only when FD is actually switched on. A controller keeps its last
        // data-phase timing after FD is turned off, and reporting that as the
        // live rate would describe a bus carrying eight-byte frames as if it
        // were doing 2 Mbit/s.
        if (state->fdEnabled.value_or(false) && state->dataBps.has_value())
        {
            actual.dataBps = *state->dataBps;
            actual.dataSamplePointPermille = state->dataSamplePointPermille.value_or(0);
        }
        else
        {
            actual.dataBps = 0;
        }
        return actual;
    }

    // Whether the controller CAN do FD, which is what the name says, rather
    // than whether this socket asked for FD frames. The two differ on every
    // FD adapter opened at a classic bit rate, and enumerate() reported the
    // hardware's answer while this reported the socket's.
    bool supports_fd() const override
    {
        auto state = link_state();
        return state.has_value() ? state->fdCapable : fdFrames_;
    }

    Result<void> set_listen_only(bool listenOnly) override
    {
        listenOnly_ = listenOnly;
        return set_bitrate(bitrate_);
    }

    bool listen_only() const override
    {
        auto state = link_state();
        if (state.has_value() && state->listenOnly.has_value())
        {
            return *state->listenOnly;
        }
        return listenOnly_;
    }

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
        invalidate_state();
        return result;
    }

    Result<void> stop() override
    {
        LinkRequest down;
        down.interface = id_.device;
        down.up = false;
        auto result = send_link_request(down);
        running_ = false;
        invalidate_state();
        return result;
    }

    bool running() const override
    {
        auto state = link_state();
        return state.has_value() ? state->up : running_.load();
    }

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

        // send() is documented safe to call while another thread is in
        // receive(), and both move these counters. The write itself is atomic
        // per frame on a CAN_RAW socket and needs no lock; the arithmetic
        // after it does. The pcan backend has always taken this lock -- this
        // one was reading and writing the same struct from three threads.
        std::lock_guard<std::mutex> lock(statsMutex_);
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
        // Accumulated here and applied once below, so a busy bus does not take
        // and drop the lock several thousand times a second.
        uint64_t rxBytes = 0;
        uint64_t errorFrames = 0;
        uint64_t busOffs = 0;
        // Frames that would not decode. Counted rather than logged: this is the
        // per-frame path, and a bus producing malformed frames produces them at
        // bus rate -- the log line cost more than the decode and buried every
        // other line in the file. rxDropped already reaches the dashboard
        // through CanBridgeChannelStatus, which is where a hole in the capture
        // should be visible anyway.
        uint64_t undecodable = 0;

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
                ++undecodable;
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
                ++errorFrames;
                // An error frame's identifier carries the error class bits.
                // This is the fallback for a kernel whose link reply has no
                // xstats block; statistics() prefers the kernel's count.
                if ((frame->id & CAN_ERR_BUSOFF) != 0)
                {
                    ++busOffs;
                }
            }
            rxBytes += frame->len;
            out[count++] = *frame;
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            statistics_.rxFrames += count;
            statistics_.rxBytes += rxBytes;
            statistics_.errorFrames += errorFrames;
            statistics_.busOffCount += busOffs;
            statistics_.rxDropped += undecodable;
        }

        return count;
    }

    Statistics statistics() const override
    {
        Statistics copy;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            copy = statistics_;
        }

        auto state = link_state();
        if (!state.has_value())
        {
            copy.state = running_ ? BusState::Unknown : BusState::Stopped;
            return copy;
        }

        copy.state = state->state.has_value() ? to_bus_state(*state->state)
                                              : (state->up ? BusState::Unknown : BusState::Stopped);
        // The controller's own counters, which is the only place they exist --
        // nothing on the receive path can see them.
        if (state->rxErrorCounter.has_value())
        {
            copy.rxErrorCounter = clamp_counter(*state->rxErrorCounter);
        }
        if (state->txErrorCounter.has_value())
        {
            copy.txErrorCounter = clamp_counter(*state->txErrorCounter);
        }
        // The kernel counts bus-off events for the lifetime of the interface,
        // which is strictly better than counting the error frames this socket
        // happened to be listening for, so it wins where it is available.
        if (state->busOffCount.has_value())
        {
            copy.busOffCount = *state->busOffCount;
        }
        return copy;
    }

    void set_fd_frames(bool enabled) { fdFrames_ = enabled; }
    void set_running(bool running) { running_ = running; }

private:
    static uint8_t clamp_counter(uint16_t value)
    {
        // The controller's counters are 8 bits; the kernel's struct is 16.
        return static_cast<uint8_t>(value > 255u ? 255u : value);
    }

    static BusState to_bus_state(CanState state)
    {
        switch (state)
        {
        case CanState::ErrorActive: return BusState::ErrorActive;
        case CanState::ErrorWarning: return BusState::ErrorWarning;
        case CanState::ErrorPassive: return BusState::ErrorPassive;
        case CanState::BusOff: return BusState::BusOff;
        case CanState::Stopped: return BusState::Stopped;
        // Nothing here puts a controller to sleep, but the kernel can report
        // it, and it is not running when it does.
        case CanState::Sleeping: return BusState::Stopped;
        }
        return BusState::Unknown;
    }

    // One round trip serves every accessor for a moment. The bridge's status
    // publish asks five separate questions once a second and each would
    // otherwise be its own socket, sendto and recv.
    std::optional<LinkState> link_state() const
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!cached_.has_value() || now - cachedAt_ >= kStateCacheFor)
        {
            auto queried = query_link(id_.device);
            if (queried.has_value())
            {
                cached_ = std::move(*queried);
                cachedAt_ = now;
            }
            else
            {
                // Keep whatever was last known rather than dropping to
                // "unknown" on one failed syscall. A query that has never
                // worked leaves this nullopt and every accessor falls back to
                // what was asked for, which is what this class did before it
                // could read anything at all.
                SPDLOG_DEBUG("[socketcan] cannot read {}'s state: {}", id_.device,
                             to_string(queried.error()));
                cachedAt_ = now;
            }
        }
        return cached_;
    }

    void invalidate_state()
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cachedAt_ = {};
    }

    static constexpr std::chrono::milliseconds kStateCacheFor { 50 };

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

    // Separate from stateMutex_ on purpose: statistics() does a netlink round
    // trip under that one, and the receive loop must not wait on a syscall to
    // record a frame it already has.
    mutable std::mutex statsMutex_;

    mutable std::mutex stateMutex_;
    mutable std::optional<LinkState> cached_;
    mutable std::chrono::steady_clock::time_point cachedAt_ {};
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

            // Asked rather than assumed. This used to claim FD for every CAN
            // interface on the machine, so a classic controller advertised a
            // capability that only failed at open.
            auto state = query_link(info.id.device);
            if (state.has_value())
            {
                info.supportsFd = state->fdCapable;
                // A down interface is not a problem for a process that can
                // bring it up, which is exactly what open() does. It is a
                // hard stop for one that cannot, and saying so here beats a
                // bridge that starts and then carries nothing.
                if (!state->up && !have_net_admin())
                {
                    info.available = false;
                    info.unavailableReason = fmt::format(
                        "the interface is down and this process has no CAP_NET_ADMIN to bring "
                        "it up; 'sudo ip link set {} up type can bitrate <rate>'",
                        info.id.device);
                }
            }
            else
            {
                info.supportsFd = true;
            }

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

        // Error frames are not delivered unless they are asked for: a
        // CAN_RAW socket's error mask starts empty, so without this the
        // controller can go bus-off and this socket sees nothing at all. That
        // made Statistics::errorFrames and the whole isError path dead code.
        // Best-effort, because losing them costs visibility rather than
        // traffic.
        const can_err_mask_t errorMask = CAN_ERR_MASK;
        if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errorMask, sizeof(errorMask)) < 0)
        {
            SPDLOG_DEBUG("[socketcan] {} will not report error frames: {}", id.device,
                         std::strerror(errno));
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
                    // The interface may well already be up, in which case
                    // reading and writing works regardless -- but that has to
                    // be checked rather than assumed. Believing it was the
                    // difference between a bridge that says it cannot start
                    // and one that publishes "up at 500 kbit/s" for a dead
                    // bus and only mentions the problem in a single warning.
                    channel->set_running(true);
                    if (!channel->running())
                    {
                        return permission_denied(fmt::format(
                            "{} is down and bringing it up needs CAP_NET_ADMIN. Configure it "
                            "first with 'sudo ip link set {} up type can bitrate {}'{}, or give "
                            "this process the capability",
                            id.device, id.device, options.bitrate.nominalBps,
                            options.bitrate.fd()
                                ? fmt::format(" dbitrate {} fd on", options.bitrate.dataBps)
                                : ""));
                    }
                    SPDLOG_WARN("[socketcan] cannot configure {}: {}", id.device,
                                started.error().message);
                    SPDLOG_WARN("[socketcan] it is already up, so carrying on with it as it is");
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
