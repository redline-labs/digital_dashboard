// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/ncm_bridge.py
#include "apple_usb/ncm_bridge.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;

namespace apple_usb
{

namespace
{

// NTB16 signatures, little-endian as they appear on the wire.
constexpr uint32_t kNth16Signature = 0x484D434E;  // "NCMH"
constexpr uint32_t kNdp16Signature = 0x304D434E;  // "NCM0"
// The last signature byte is '0' (no CRC) or '1' (CRC); accept either.
constexpr uint32_t kNdp16SignatureMask = 0x00FFFFFF;

constexpr size_t kNth16Length = 12;   // wHeaderLength we emit
constexpr size_t kNdp16Length = 16;   // NDP16 header + 1 entry + terminator
constexpr size_t kTxDatagramOffset = kNth16Length + kNdp16Length;  // 28

// CDC class requests. The interface class codes, the descriptor sub-types and
// the data altsetting live in ncm_discovery.h now, alongside the descriptor
// parsing that is the only thing which needed them.
constexpr uint8_t kGetNtbParameters = 0x80;
// CDC class requests the kernel's cdc_ncm driver issues during bring-up. We
// previously sent only GET_NTB_PARAMETERS, which is the one request that reads
// state rather than establishing any.
constexpr uint8_t kSetEthernetPacketFilter = 0x43;
constexpr uint8_t kSetNtbFormat = 0x84;
constexpr uint8_t kSetNtbInputSize = 0x86;
constexpr uint16_t kNtbFormat16 = 0x0000;
// DIRECTED | BROADCAST | ALL_MULTICAST | PROMISCUOUS.
constexpr uint16_t kPacketFilterAll = 0x000F;

// Transfer sizing (matches ncm_bridge.py).
constexpr size_t kUsbReadSize = 32768;
constexpr size_t kTapReadSize = 4096;
constexpr uint32_t kNtbOutMaxCeiling = 32764;
constexpr unsigned kUsbReadTimeoutMs = 2000;
constexpr unsigned kUsbWriteTimeoutMs = 3000;
constexpr int kTapPollTimeoutMs = 1000;

std::string readSysfsAttr(const fs::path& dir, const char* attr)
{
    std::ifstream in(dir / attr);
    std::string value;
    std::getline(in, value);
    // sysfs attributes are newline terminated; getline already dropped it, but
    // trim stray whitespace defensively.
    while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
    {
        value.pop_back();
    }
    return value;
}

uint16_t get_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put_le16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_le32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// TAP devices are named cpusb0, cpusb1, ... -- the lowest name not already in
// use by another live bridge. Deliberately not a counter that only goes up:
// stop() destroys the device, so a phone that is unplugged and plugged back in
// must be able to take cpusb0 again. Users without CAP_NET_ADMIN pre-create a
// persistent cpusb0 to attach to (see docs/carplay_bringup.md stage 6), and a
// counter would walk past it on the second session.
constexpr unsigned kMaxTapIndex = 8;

// Run a command, logging the exact argv and any output. Returns the exit
// status, or -1 when the child could not be started / was killed.
// <linux/ipv6.h> cannot be included alongside <netinet/in.h>, so mirror the one
// structure we need from it.
struct In6Ifreq
{
    struct in6_addr ifr6_addr;
    uint32_t ifr6_prefixlen;
    int ifr6_ifindex;
};

// Parse "aa:bb:cc:dd:ee:ff" into six bytes.
bool parseMacBytes(const std::string& mac, uint8_t out[6])
{
    unsigned values[6] = {};
    if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
                    &values[3], &values[4], &values[5]) != 6)
    {
        return false;
    }
    for (int i = 0; i < 6; ++i)
    {
        out[i] = static_cast<uint8_t>(values[i]);
    }
    return true;
}

// The three interface operations below replace shell-outs to `ip`. That is not
// tidiness: file capabilities set with setcap are *not* inherited by child
// processes, so an `ip` subprocess would run unprivileged and fail with
// EPERM even though this process holds CAP_NET_ADMIN. Doing the work in-process
// keeps a single setcap on the driver binary sufficient.
bool setInterfaceMac(const std::string& ifname, const std::string& mac, int tun_fd)
{
    uint8_t bytes[6] = {};
    if (!parseMacBytes(mac, bytes))
    {
        SPDLOG_ERROR("[ncm] cannot parse MAC '{}'", mac);
        return false;
    }

    ifreq request{};
    std::strncpy(request.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    request.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    std::memcpy(request.ifr_hwaddr.sa_data, bytes, sizeof(bytes));

    // The tun driver services SIOCSIFHWADDR on its own fd and only requires
    // that we own the device, so this succeeds without CAP_NET_ADMIN on a
    // persistent TAP created for this user. Try it before the generic socket
    // path, which does need the capability.
    if (tun_fd >= 0 && ::ioctl(tun_fd, SIOCSIFHWADDR, &request) >= 0)
    {
        return true;
    }

    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        SPDLOG_ERROR("[ncm] socket() failed: {}", strerror(errno));
        return false;
    }
    const bool ok = ::ioctl(sock, SIOCSIFHWADDR, &request) >= 0;
    if (!ok)
    {
        SPDLOG_ERROR("[ncm] setting {} MAC to {} failed: {}. Either grant CAP_NET_ADMIN or "
                     "create the TAP as a persistent device owned by this user.",
                     ifname, mac, strerror(errno));
    }
    ::close(sock);
    return ok;
}

bool setInterfaceUp(const std::string& ifname)
{
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        return false;
    }

    ifreq request{};
    std::strncpy(request.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    bool ok = ::ioctl(sock, SIOCGIFFLAGS, &request) >= 0;

    // Setting the flags needs CAP_NET_ADMIN, but reading them does not. A
    // persistent TAP that was brought up once at setup time is already in the
    // desired state, so do not spend a privileged call to re-assert it.
    if (ok && (request.ifr_flags & IFF_UP) != 0)
    {
        SPDLOG_DEBUG("[ncm] {} is already up", ifname);
        ::close(sock);
        return true;
    }

    if (ok)
    {
        request.ifr_flags |= (IFF_UP | IFF_RUNNING);
        ok = ::ioctl(sock, SIOCSIFFLAGS, &request) >= 0;
    }
    if (!ok)
    {
        SPDLOG_ERROR("[ncm] bringing {} up failed: {}. Either grant CAP_NET_ADMIN or bring the "
                     "persistent TAP up once at setup ('ip link set {} up').",
                     ifname, strerror(errno), ifname);
    }
    ::close(sock);
    return ok;
}

// /proc/net/if_inet6 lists every IPv6 address per interface as 32 hex chars.
// Readable unprivileged, which is what lets us confirm the kernel already
// assigned the EUI-64 link-local we were about to add.
bool hasIpv6Address(const std::string& ifname, const std::string& address)
{
    in6_addr wanted{};
    if (::inet_pton(AF_INET6, address.c_str(), &wanted) != 1)
    {
        return false;
    }

    std::ifstream in("/proc/net/if_inet6");
    std::string hex, index, prefix, scope, flags, name;
    while (in >> hex >> index >> prefix >> scope >> flags >> name)
    {
        if (name != ifname || hex.size() != 32)
        {
            continue;
        }
        in6_addr candidate{};
        bool parsed = true;
        for (int i = 0; i < 16 && parsed; ++i)
        {
            unsigned byte = 0;
            const char* start = hex.data() + (i * 2);
            parsed = std::from_chars(start, start + 2, byte, 16).ec == std::errc{};
            candidate.s6_addr[i] = static_cast<uint8_t>(byte);
        }
        if (parsed && std::memcmp(&candidate, &wanted, sizeof(wanted)) == 0)
        {
            return true;
        }
    }
    return false;
}

// The link-local the interface actually has, for diagnostics: naming the wrong
// address is what makes the "which MAC was this derived from?" question
// answerable at a glance.
std::string firstLinkLocal(const std::string& ifname)
{
    std::ifstream in("/proc/net/if_inet6");
    std::string hex, index, prefix, scope, flags, name;
    while (in >> hex >> index >> prefix >> scope >> flags >> name)
    {
        if (name != ifname || hex.size() != 32 || hex.compare(0, 4, "fe80") != 0)
        {
            continue;
        }
        in6_addr addr{};
        bool parsed = true;
        for (int i = 0; i < 16 && parsed; ++i)
        {
            unsigned byte = 0;
            const char* start = hex.data() + (i * 2);
            parsed = std::from_chars(start, start + 2, byte, 16).ec == std::errc{};
            addr.s6_addr[i] = static_cast<uint8_t>(byte);
        }
        char text[INET6_ADDRSTRLEN] = {};
        if (parsed && ::inet_ntop(AF_INET6, &addr, text, sizeof(text)) != nullptr)
        {
            return text;
        }
    }
    return {};
}

// The kernel adds an address asynchronously once the interface has carrier, so
// a check made immediately after the triggering event races it.
bool waitForIpv6Address(const std::string& ifname, const std::string& address,
                        std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (hasIpv6Address(ifname, address))
        {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        ::poll(nullptr, 0, 10);
    }
}

bool addIpv6Address(const std::string& ifname, const std::string& address, uint32_t prefix_len)
{
    const int sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        SPDLOG_ERROR("[ncm] AF_INET6 socket() failed: {}", strerror(errno));
        return false;
    }

    ifreq index_request{};
    std::strncpy(index_request.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (::ioctl(sock, SIOCGIFINDEX, &index_request) < 0)
    {
        SPDLOG_ERROR("[ncm] SIOCGIFINDEX({}) failed: {}", ifname, strerror(errno));
        ::close(sock);
        return false;
    }

    In6Ifreq request{};
    if (::inet_pton(AF_INET6, address.c_str(), &request.ifr6_addr) != 1)
    {
        SPDLOG_ERROR("[ncm] cannot parse IPv6 address '{}'", address);
        ::close(sock);
        return false;
    }
    request.ifr6_prefixlen = prefix_len;
    request.ifr6_ifindex = index_request.ifr_ifindex;

    // EEXIST means the kernel already autoconfigured this exact link-local,
    // which is the desired end state.
    bool ok = ::ioctl(sock, SIOCSIFADDR, &request) >= 0 || errno == EEXIST;
    const int saved_errno = errno;
    ::close(sock);

    if (ok)
    {
        return true;
    }

    // Without CAP_NET_ADMIN we cannot add it -- but with addrgenmode eui64 the
    // kernel derives this exact address itself when the interface is brought
    // up, so on a correctly set up persistent TAP it is already there. Allow a
    // moment for it in case we raced address generation.
    if (waitForIpv6Address(ifname, address, std::chrono::milliseconds(500)))
    {
        SPDLOG_DEBUG("[ncm] {} already carries {}, kernel-assigned", ifname, address);
        return true;
    }

    // Do not suggest 'addrgenmode eui64' here: it is almost always already set,
    // and saying so sends the reader in a circle -- that was the previous
    // wording and it cost an hour. The address is derived from the MAC at
    // bring-up, so what actually breaks it is the MAC being set afterwards,
    // which leaves a link-local derived from the TAP's original random MAC.
    // The kernel will not replace it: addresses survive carrier loss, and
    // addrconf neither regenerates on a MAC change nor adds a second
    // link-local.
    const std::string have = firstLinkLocal(ifname);
    SPDLOG_ERROR("[ncm] adding {}/{} to {} failed: {}, and the kernel has not derived it "
                 "either -- {} carries {} instead, which is derived from a different MAC. "
                 "Pin the phone's MAC on the persistent TAP before it is brought up "
                 "(CARPLAY_TAP_MAC in carplay-tap.service, then restart it), or run as "
                 "root. See docs/carplay_bringup.md stage 6.",
                 address, prefix_len, ifname, strerror(saved_errno), ifname,
                 have.empty() ? std::string("no link-local") : have);
    return false;
}

int runCommand(const std::vector<std::string>& argv, int timeout_ms = 5000)
{
    std::string joined;
    for (const auto& a : argv)
    {
        if (!joined.empty())
        {
            joined += ' ';
        }
        joined += a;
    }
    SPDLOG_INFO("[ncm] exec: {}", joined);

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0)
    {
        SPDLOG_WARN("[ncm] pipe() failed for '{}': {}", joined, strerror(errno));
        return -1;
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        SPDLOG_WARN("[ncm] fork() failed for '{}': {}", joined, strerror(errno));
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0)
    {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv)
        {
            args.push_back(const_cast<char*>(a.c_str()));
        }
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    ::close(pipefd[1]);
    std::string output;
    bool killed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0)
        {
            if (!killed)
            {
                SPDLOG_WARN("[ncm] '{}' exceeded {} ms; killing", joined, timeout_ms);
                ::kill(pid, SIGKILL);
                killed = true;
            }
            remaining = 500;
        }
        pollfd p{};
        p.fd = pipefd[0];
        p.events = POLLIN;
        const int n = ::poll(&p, 1, static_cast<int>(remaining));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (n == 0)
        {
            continue;
        }
        char buf[512];
        const ssize_t r = ::read(pipefd[0], buf, sizeof(buf));
        if (r <= 0)
        {
            break;
        }
        output.append(buf, static_cast<size_t>(r));
    }
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    {
        output.pop_back();
    }

    const int rc = (!killed && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    if (rc != 0)
    {
        SPDLOG_WARN("[ncm] '{}' exited {}{}{}", joined, rc, output.empty() ? "" : ": ", output);
    }
    else if (!output.empty())
    {
        SPDLOG_DEBUG("[ncm] '{}' output: {}", joined, output);
    }
    return rc;
}

// EUI-64 IPv6 link-local from a MAC, exactly as LIVI's
// cp_handler._iface_eui64_fe80: flip the universal/local bit of the first
// octet and insert ff:fe in the middle.
std::string deriveEui64LinkLocal(const std::string& mac)
{
    unsigned b[6] = {0, 0, 0, 0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (pos + 2 > mac.size())
        {
            return {};
        }
        unsigned value = 0;
        const auto* first = mac.data() + pos;
        if (std::from_chars(first, first + 2, value, 16).ec != std::errc{})
        {
            return {};
        }
        b[i] = value;
        pos += 2;
        if (i < 5)
        {
            if (pos >= mac.size() || mac[pos] != ':')
            {
                return {};
            }
            ++pos;
        }
    }
    if (pos != mac.size())
    {
        return {};
    }
    return fmt::format("fe80::{:x}:{:x}:{:x}:{:x}", ((b[0] ^ 0x02u) << 8) | b[1],
                       (b[2] << 8) | 0xffu, (0xfeu << 8) | b[3], (b[4] << 8) | b[5]);
}

}  // namespace

NcmBridge::NcmBridge(DeviceInfo device) : device_(std::move(device)) {}

NcmBridge::~NcmBridge()
{
    stop();
}

// ---------------- discovery ----------------

void NcmBridge::detachKernelNcmDrivers()
{
    // In the CarPlay configuration the phone exposes *two* NCM function pairs,
    // and the kernel's cdc_ncm binds the first one the instant the
    // configuration is applied. Left alone that costs us twice: claiming would
    // fight the driver, and selectNcmFunction() would have to skip the
    // driver-owned interfaces and silently pick the *second* pair instead of
    // the one LIVI uses.
    const auto config = readActiveConfig(device_);
    if (!config)
    {
        SPDLOG_WARN("[ncm] cannot read the configuration descriptor at port {}",
                    device_.port.toString());
        return;
    }

    unsigned released = 0;
    for (const auto& fn : findNcmFunctions(*config))
    {
        for (const uint8_t iface : {fn.ctrl_iface, fn.data_iface})
        {
            if (!kernelDriverActive(handle_, iface))
            {
                continue;
            }
            if (detachKernelDriver(handle_, iface))
            {
                SPDLOG_INFO("[ncm] released interface {} from its kernel driver", iface);
                ++released;
            }
            else
            {
                SPDLOG_WARN("[ncm] could not release interface {} from its kernel driver", iface);
            }
        }
    }

    if (released > 0)
    {
        // Unbinding tears the netdev down asynchronously; give the kernel a
        // moment to catch up before we try to claim.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool NcmBridge::selectNcmFunction()
{
    const auto config = readActiveConfig(device_);
    if (!config)
    {
        SPDLOG_ERROR("[ncm] cannot read the configuration descriptor at port {}",
                     device_.port.toString());
        return false;
    }

    std::optional<NcmFunction> chosen;

    // Bring-up override. The CarPlay configuration exposes two NCM function
    // pairs and which one carries the AV link is not self-evident from the
    // descriptors, so allow pinning the control interface while that is being
    // established.
    if (const char* pinned = std::getenv("CARPLAY_NCM_CTRL_IF"); pinned != nullptr)
    {
        unsigned value = 0;
        if (std::from_chars(pinned, pinned + std::strlen(pinned), value).ec == std::errc{})
        {
            chosen = findNcmFunctionByCtrl(*config, static_cast<uint8_t>(value));
            if (!chosen)
            {
                SPDLOG_ERROR("[ncm] CARPLAY_NCM_CTRL_IF={} names no NCM function in "
                             "configuration {}", value, config->value);
                return false;
            }
            SPDLOG_WARN("[ncm] CARPLAY_NCM_CTRL_IF pins the NCM pair to {}/{}",
                        chosen->ctrl_iface, chosen->data_iface);
        }
    }

    if (!chosen)
    {
        const auto functions = findNcmFunctions(*config);
        if (functions.empty())
        {
            SPDLOG_ERROR("[ncm] no NCM function pair in configuration {}", config->value);
            return false;
        }
        if (functions.size() > 1)
        {
            SPDLOG_INFO("[ncm] {} NCM function pairs present; taking the first (control "
                        "interface {}). Override with CARPLAY_NCM_CTRL_IF.",
                        functions.size(), functions.front().ctrl_iface);
        }
        chosen = functions.front();
    }

    // A driver still holding either interface means detachKernelNcmDrivers()
    // did not manage it. Claiming would fail with EBUSY a moment later; say so
    // now, with the reason.
    for (const uint8_t iface : {chosen->ctrl_iface, chosen->data_iface})
    {
        if (kernelDriverActive(handle_, iface))
        {
            SPDLOG_ERROR("[ncm] refusing to claim: a kernel driver still holds interface {} "
                         "of the selected NCM pair", iface);
            return false;
        }
    }

    if (!chosen->hasBulkPair())
    {
        SPDLOG_ERROR("[ncm] NCM function {}/{} exposes no bulk pair on data altsetting {} "
                     "(in=0x{:02x} out=0x{:02x})", chosen->ctrl_iface, chosen->data_iface,
                     kNcmDataAltSetting, chosen->ep_in, chosen->ep_out);
        return false;
    }

    ctrl_iface_ = chosen->ctrl_iface;
    data_iface_ = chosen->data_iface;
    ep_in_ = chosen->ep_in;
    ep_out_ = chosen->ep_out;
    ep_int_ = chosen->ep_int;
    mac_string_index_ = chosen->mac_string_index;

    SPDLOG_INFO("[ncm] NCM pair in configuration {}: control iface {} (status ep 0x{:02x}), "
                "data iface {} (bulk in 0x{:02x} / out 0x{:02x}), iMACAddress string {}",
                config->value, ctrl_iface_, ep_int_, data_iface_, ep_in_, ep_out_,
                mac_string_index_);
    if (ep_int_ == 0)
    {
        SPDLOG_WARN("[ncm] no interrupt endpoint on control interface {}", ctrl_iface_);
    }
    return true;
}

std::string NcmBridge::readHostMac(uint8_t mac_string_index) const
{
    if (mac_string_index == 0)
    {
        SPDLOG_WARN("[ncm] no CDC Ethernet functional descriptor for iface {}", ctrl_iface_);
        return {};
    }

    std::vector<uint8_t> descriptor;
    try
    {
        // GET_DESCRIPTOR(STRING, mac_string_index), langid 0x0409.
        descriptor = usbControl(handle_, 0x80, 6,
                                static_cast<uint16_t>((3u << 8) | mac_string_index), 0x0409, 64);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_WARN("[ncm] GET_DESCRIPTOR(string {}) failed: {}", mac_string_index, e.what());
        return {};
    }

    const std::string mac = macFromStringDescriptor(descriptor);
    if (mac.empty())
    {
        SPDLOG_WARN("[ncm] iMACAddress string {} is not a 12-character hex MAC ({} bytes)",
                    mac_string_index, descriptor.size());
    }
    return mac;
}

// ---------------- setup ----------------

bool NcmBridge::createTap()
{
    tap_fd_ = ::open("/dev/net/tun", O_RDWR);
    if (tap_fd_ < 0)
    {
        SPDLOG_ERROR("[ncm] open(/dev/net/tun) failed: {}", strerror(errno));
        return false;
    }

    // Attaches to an existing persistent TAP of this name if one is owned by
    // us, and only tries to create a new device otherwise -- creation is what
    // needs CAP_NET_ADMIN, attaching does not. A name another live bridge is
    // already attached to comes back EBUSY, so walk up until one takes.
    ifreq ifr{};
    int err = 0;
    for (unsigned index = 0; index < kMaxTapIndex; ++index)
    {
        ifname_ = fmt::format("cpusb{}", index);
        ifr = ifreq{};
        std::strncpy(ifr.ifr_name, ifname_.c_str(), IFNAMSIZ - 1);
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        if (::ioctl(tap_fd_, TUNSETIFF, &ifr) == 0)
        {
            err = 0;
            break;
        }
        err = errno;
        if (err != EBUSY)
        {
            break;  // not a name clash; another name will not help
        }
    }
    if (err != 0)
    {
        SPDLOG_ERROR("[ncm] TUNSETIFF({}) failed: {}. Create a persistent TAP owned by this "
                     "user once -- 'ip tuntap add dev cpusb0 mode tap user $USER' -- or grant "
                     "CAP_NET_ADMIN. See docs/carplay_bringup.md stage 6.",
                     ifname_, strerror(err));
        return false;
    }
    // The kernel may hand back a different name if ours collided.
    ifname_.assign(ifr.ifr_name, ::strnlen(ifr.ifr_name, IFNAMSIZ));
    SPDLOG_INFO("[ncm] TAP device {} created (IFF_TAP|IFF_NO_PI)", ifname_);
    return true;
}

bool NcmBridge::configureInterface(const std::string& mac)
{
    if (!mac.empty())
    {
        // The phone dictates the host MAC through iMACAddress; it will not
        // talk to us if we use the random one the kernel generated. Setting it
        // late is fine for the MAC itself -- it is only the link-local the
        // kernel already derived that cannot be revised, and we no longer
        // depend on that address matching this MAC.
        setInterfaceMac(ifname_, mac, tap_fd_);
    }
    else
    {
        SPDLOG_WARN("[ncm] no host MAC from the CDC Ethernet descriptor; keeping the "
                    "kernel-assigned TAP address (the phone may ignore us)");
    }

    // Keep NetworkManager off this link; it would otherwise try DHCP and
    // rewrite addresses underneath us. Best effort: nmcli may not exist.
    runCommand({"nmcli", "device", "set", ifname_, "managed", "no"}, 10000);

    // One peer on this link, so duplicate address detection is pointless and
    // would only delay the fe80 address becoming usable.
    {
        const std::string dad_path = fmt::format("/proc/sys/net/ipv6/conf/{}/accept_dad", ifname_);
        std::ofstream out(dad_path);
        if (out)
        {
            out << "0";
            SPDLOG_INFO("[ncm] wrote 0 to {}", dad_path);
        }
        else
        {
            // Root-owned, so this fails on the unprivileged path -- but
            // carplay-tap.service already sets it on the persistent TAP. Only
            // warn when it is genuinely still enabled, otherwise this fires on
            // every run and trains the reader to ignore [ncm] warnings.
            std::ifstream in(dad_path);
            std::string current;
            if (in && (in >> current) && current == "0")
            {
                SPDLOG_DEBUG("[ncm] {} already 0, left alone", dad_path);
            }
            else
            {
                SPDLOG_WARN("[ncm] cannot write {} (DAD stays enabled)", dad_path);
            }
        }
    }

    if (!setInterfaceUp(ifname_))
    {
        return false;
    }

    // A MAC change here means the link-local the kernel derived when this
    // interface was brought up is now stale, and nothing we can do without
    // CAP_NET_ADMIN will dislodge it: addresses survive carrier loss (only
    // NETDEV_DOWN flushes them), addrconf does not regenerate on
    // NETDEV_CHANGEADDR, and it will not add a second link-local when one
    // already exists. Bouncing carrier via TUNSETCARRIER was tried and does
    // nothing for exactly that reason -- do not re-attempt it.
    //
    // The supported unprivileged answer is to pin this MAC on the persistent
    // TAP *before* it is first brought up, which carplay-tap.service does; the
    // kernel then derives the right address at boot and this call is a no-op.
    host_mac_ = readSysfsAttr(fs::path("/sys/class/net") / ifname_, "address");
    const std::string& actual_mac = host_mac_;

    // What we advertise only has to be an address the phone can reach us on.
    // It goes into CarPlayStartSession and the phone dials it at :7000; NDP
    // then resolves it to whatever MAC we are presenting. So prefer whatever
    // link-local the kernel has already put on the interface -- that address is
    // reachable by definition, and taking it means we never need to add one,
    // which is the only step here that wanted CAP_NET_ADMIN.
    //
    // The EUI-64 derivation below is the fallback for an interface that has no
    // link-local at all. It matches what LIVI ends up with, but there the match
    // is incidental: LIVI creates the TAP with the phone's MAC, so the kernel
    // derives from it anyway.
    const std::string derived = deriveEui64LinkLocal(actual_mac);
    std::string existing;
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        do
        {
            existing = firstLinkLocal(ifname_);
        } while (existing.empty() && std::chrono::steady_clock::now() < deadline &&
                 ::poll(nullptr, 0, 10) >= 0);
    }

    if (!existing.empty())
    {
        fe80_ = existing;
        if (!derived.empty() && existing != derived)
        {
            // Normal on a persistent TAP whose MAC was pinned after bring-up.
            // Worth one line because it is the difference between this and the
            // address a reader would predict from the MAC.
            SPDLOG_INFO("[ncm] {} mac={} -> advertising kernel link-local {} (EUI-64 of this "
                        "MAC would be {}; the kernel derived from the MAC the interface had "
                        "when it was brought up)",
                        ifname_, actual_mac, fe80_, derived);
        }
        else
        {
            SPDLOG_INFO("[ncm] {} mac={} -> link-local {}", ifname_, actual_mac, fe80_);
        }
        return true;
    }

    fe80_ = derived;
    if (fe80_.empty())
    {
        SPDLOG_ERROR("[ncm] cannot derive fe80 from {} MAC '{}'", ifname_, actual_mac);
        return false;
    }
    SPDLOG_INFO("[ncm] {} mac={} -> link-local {} (deriving; interface had none)", ifname_,
                actual_mac, fe80_);
    if (!addIpv6Address(ifname_, fe80_, 64))
    {
        return false;
    }
    return true;
}

bool NcmBridge::start()
{
    if (run_.load())
    {
        SPDLOG_WARN("[ncm] start() called while already running on {}", ifname_);
        return true;
    }
    if (device_.port.empty())
    {
        SPDLOG_ERROR("[ncm] device has no port path");
        return false;
    }

    handle_ = openDevice(device_);
    if (!handle_)
    {
        return false;
    }

    // Order matters: release the kernel's claim first, then read the
    // descriptors and pick a function, then claim.
    detachKernelNcmDrivers();

    if (!selectNcmFunction())
    {
        cleanup();
        return false;
    }

    try
    {
        usbClaimInterface(handle_, ctrl_iface_);
        ctrl_claimed_ = true;
        usbClaimInterface(handle_, data_iface_);
        data_claimed_ = true;
    }
    catch (const std::system_error& e)
    {
        SPDLOG_ERROR("[ncm] claiming ifaces {}/{} failed: {}", ctrl_iface_, data_iface_, e.what());
        cleanup();
        return false;
    }

    // GET_NTB_PARAMETERS (class request 0x80 on the control interface): 28
    // bytes describing the framing the device expects.
    try
    {
        const auto params = usbControl(handle_, 0xA1, kGetNtbParameters, 0,
                                       static_cast<uint16_t>(ctrl_iface_), 28);
        if (params.size() >= 28)
        {
            in_max_ = get_le32(params.data() + 4);
            out_max_ = std::min(get_le32(params.data() + 16), kNtbOutMaxCeiling);
            SPDLOG_INFO("[ncm] NTB params: formats=0x{:04x} inMax={} outMax={} (clamped {}) "
                        "ndpOutDivisor={} ndpOutRemainder={} ndpOutAlign={} maxDatagrams={}",
                        get_le16(params.data() + 2), get_le32(params.data() + 4),
                        get_le32(params.data() + 16), out_max_, get_le16(params.data() + 20),
                        get_le16(params.data() + 22), get_le16(params.data() + 24),
                        get_le16(params.data() + 26));
        }
        else
        {
            SPDLOG_WARN("[ncm] GET_NTB_PARAMETERS returned {} bytes (expected 28); using outMax={}",
                        params.size(), out_max_);
        }
    }
    catch (const std::system_error& e)
    {
        SPDLOG_WARN("[ncm] GET_NTB_PARAMETERS failed ({}); using outMax={}", e.what(), out_max_);
    }

    const std::string mac = readHostMac(mac_string_index_);

    // Activate the data altsetting. The bulk endpoints only *exist* there --
    // but unlike the sysfs version this replaces, their addresses were already
    // read from the descriptor, so selecting the altsetting is all this does.
    try
    {
        usbSetAltSetting(handle_, data_iface_, kNcmDataAltSetting);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_ERROR("[ncm] selecting altsetting {} on interface {} failed: {}",
                     kNcmDataAltSetting, data_iface_, e.what());
        cleanup();
        return false;
    }

    // We take these endpoints over from the kernel's cdc_ncm driver, which may
    // leave their data toggles advanced. A toggle we disagree with makes the
    // device discard everything we send as a duplicate and NAK it, so bulk
    // writes time out while reads on the same interface keep working. Clearing
    // halt resets the toggle on both sides.
    usbClearHalt(handle_, ep_in_);
    usbClearHalt(handle_, ep_out_);

    // Complete the CDC-NCM bring-up handshake. Skipping these leaves the device
    // in a state where it will happily stream to us but never accepts anything
    // on the bulk OUT endpoint, which surfaces as every write timing out.
    // bmRequestType 0x21 = host-to-device, class, interface.
    try
    {
        usbControl(handle_, 0x21, kSetNtbFormat, kNtbFormat16,
                   static_cast<uint16_t>(ctrl_iface_), 0);
        SPDLOG_DEBUG("[ncm] SET_NTB_FORMAT(NTB16) ok");
    }
    catch (const std::system_error& e)
    {
        SPDLOG_DEBUG("[ncm] SET_NTB_FORMAT failed ({}); device may be NTB16-only", e.what());
    }

    try
    {
        const uint32_t input_size = in_max_;
        const uint8_t payload[4] = {
            static_cast<uint8_t>(input_size & 0xFF),
            static_cast<uint8_t>((input_size >> 8) & 0xFF),
            static_cast<uint8_t>((input_size >> 16) & 0xFF),
            static_cast<uint8_t>((input_size >> 24) & 0xFF)};
        usbControl(handle_, 0x21, kSetNtbInputSize, 0, static_cast<uint16_t>(ctrl_iface_),
                   sizeof(payload), payload);
        SPDLOG_DEBUG("[ncm] SET_NTB_INPUT_SIZE({}) ok", input_size);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_DEBUG("[ncm] SET_NTB_INPUT_SIZE failed ({})", e.what());
    }

    try
    {
        usbControl(handle_, 0x21, kSetEthernetPacketFilter, kPacketFilterAll,
                   static_cast<uint16_t>(ctrl_iface_), 0);
        SPDLOG_INFO("[ncm] SET_ETHERNET_PACKET_FILTER(0x{:04x}) ok", kPacketFilterAll);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_WARN("[ncm] SET_ETHERNET_PACKET_FILTER failed ({})", e.what());
    }

    if (!createTap() || !configureInterface(mac))
    {
        cleanup();
        return false;
    }

    run_.store(true);
    if (ep_int_ != 0)
    {
        status_thread_ = std::thread([this] { statusLoop(); });
    }
    usb_to_tap_ = std::thread([this] { usbToTapLoop(); });
    tap_to_usb_ = std::thread([this] { tapToUsbLoop(); });

    SPDLOG_INFO("[ncm] userspace NCM up on {}: iface {}/{} ep in=0x{:02x} out=0x{:02x} mac={} "
                "tap={} fe80={}",
                device_.serial, ctrl_iface_, data_iface_, ep_in_, ep_out_, mac.empty() ? "?" : mac,
                ifname_, fe80_);
    return true;
}

void NcmBridge::stop()
{
    const bool was_running = run_.exchange(false);
    if (status_thread_.joinable())
    {
        status_thread_.join();
    }
    if (usb_to_tap_.joinable())
    {
        usb_to_tap_.join();
    }
    if (tap_to_usb_.joinable())
    {
        tap_to_usb_.join();
    }
    if (was_running)
    {
        SPDLOG_INFO("[ncm] pumps stopped for {}", ifname_.empty() ? device_.serial : ifname_);
    }
    cleanup();
}

// Tear down everything acquired by start(). Safe to call repeatedly and from
// a partially-constructed state; the pumps must already be joined.
void NcmBridge::cleanup()
{
    if (tap_fd_ >= 0)
    {
        // Closing the last fd on the TUN/TAP device destroys the interface.
        ::close(tap_fd_);
        tap_fd_ = -1;
        SPDLOG_INFO("[ncm] TAP device {} destroyed", ifname_);
    }
    if (handle_)
    {
        if (data_claimed_)
        {
            usbReleaseInterface(handle_, data_iface_);
            data_claimed_ = false;
        }
        if (ctrl_claimed_)
        {
            usbReleaseInterface(handle_, ctrl_iface_);
            ctrl_claimed_ = false;
        }
        handle_.reset();
    }
    ifname_.clear();
    fe80_.clear();
}

// ---------------- NTB16 framing ----------------
//
// An NTB16 (NCM Transfer Block, 16-bit variant) is:
//
//   NTH16 @0        dwSignature "NCMH", wHeaderLength, wSequence,
//                   wBlockLength, wNdpIndex                        (12 bytes)
//   NDP16 @wNdpIndex
//                   dwSignature "NCM0"/"NCM1", wLength,
//                   wNextNdpIndex, then a datagram pointer table of
//                   (wDatagramIndex, wDatagramLength) pairs
//                   terminated by a (0, 0) entry
//   datagrams       raw ethernet frames at the offsets the table names
//
// All fields are little-endian and all offsets are from the start of the
// block.

std::vector<std::vector<uint8_t>> NcmBridge::parseNtb(const std::vector<uint8_t>& ntb) const
{
    std::vector<std::vector<uint8_t>> frames;
    const size_t total = ntb.size();
    if (total < kNth16Length)
    {
        SPDLOG_WARN("[ncm] rx NTB too short: {} bytes (< {})", total, kNth16Length);
        return frames;
    }

    const uint32_t sig = get_le32(ntb.data());
    const uint16_t header_len = get_le16(ntb.data() + 4);
    const uint16_t sequence = get_le16(ntb.data() + 6);
    const uint16_t block_len = get_le16(ntb.data() + 8);
    uint16_t ndp_idx = get_le16(ntb.data() + 10);

    if (sig != kNth16Signature)
    {
        SPDLOG_WARN("[ncm] rx NTH16 bad signature at offset 0: 0x{:08x} (want 0x{:08x}), {} bytes",
                    sig, kNth16Signature, total);
        return frames;
    }
    if (block_len > total)
    {
        SPDLOG_WARN("[ncm] rx NTH16 wBlockLength at offset 8 = {} exceeds the {} bytes received",
                    block_len, total);
    }
    SPDLOG_DEBUG("[ncm] rx NTB seq={} blockLen={} headerLen={} ndpIndex={} received={}", sequence,
                 block_len, header_len, ndp_idx, total);

    size_t ndp_count = 0;
    while (ndp_idx != 0)
    {
        if (static_cast<size_t>(ndp_idx) + 12 > total)
        {
            SPDLOG_WARN("[ncm] rx NDP16 index {} does not leave room for a 12-byte NDP in the {} "
                        "bytes received",
                        ndp_idx, total);
            break;
        }
        const uint8_t* ndp = ntb.data() + ndp_idx;
        const uint32_t nsig = get_le32(ndp);
        const uint16_t nlen = get_le16(ndp + 4);
        const uint16_t next_ndp = get_le16(ndp + 6);
        if ((nsig & kNdp16SignatureMask) != (kNdp16Signature & kNdp16SignatureMask))
        {
            SPDLOG_WARN("[ncm] rx NDP16 bad signature at offset {}: 0x{:08x} (want 0x{:08x} with "
                        "the last byte free)",
                        ndp_idx, nsig, kNdp16Signature);
            break;
        }
        if (nlen < 12)
        {
            SPDLOG_WARN("[ncm] rx NDP16 at offset {}: wLength={} is too small for a pointer table",
                        ndp_idx, nlen);
            break;
        }
        ++ndp_count;

        size_t off = static_cast<size_t>(ndp_idx) + 8;
        const size_t end = std::min<size_t>(static_cast<size_t>(ndp_idx) + nlen, total);
        size_t datagrams = 0;
        while (off + 4 <= end)
        {
            const uint16_t d_idx = get_le16(ntb.data() + off);
            const uint16_t d_len = get_le16(ntb.data() + off + 2);
            if (d_idx == 0 || d_len == 0)
            {
                // The (0, 0) terminator.
                break;
            }
            if (static_cast<size_t>(d_idx) + d_len <= total)
            {
                frames.emplace_back(ntb.begin() + d_idx, ntb.begin() + d_idx + d_len);
                ++datagrams;
            }
            else
            {
                SPDLOG_WARN("[ncm] rx datagram pointer at offset {} runs off the block: "
                            "index={} len={} (block has {} bytes)",
                            off, d_idx, d_len, total);
            }
            off += 4;
        }
        SPDLOG_DEBUG("[ncm] rx NDP16 @{} wLength={} datagrams={} nextNdpIndex={}", ndp_idx, nlen,
                     datagrams, next_ndp);

        // The spec allows a chain of NDPs; guard against a device (or a
        // corrupted block) pointing backwards, which would loop forever.
        if (next_ndp != 0 && next_ndp <= ndp_idx)
        {
            SPDLOG_WARN("[ncm] rx NDP16 @{}: wNextNdpIndex={} does not advance; stopping the chain",
                        ndp_idx, next_ndp);
            break;
        }
        ndp_idx = next_ndp;
    }
    SPDLOG_DEBUG("[ncm] rx NTB seq={} yielded {} datagram(s) from {} NDP(s)", sequence,
                 frames.size(), ndp_count);
    return frames;
}

std::vector<uint8_t> NcmBridge::buildNtb(const uint8_t* frame, size_t len)
{
    // One datagram per block: NTH16 (12) + NDP16 with a single entry and the
    // (0,0) terminator (16) = 28 bytes of framing, then the ethernet frame.
    // Both 12 and 28 are 4-byte aligned, which satisfies the wNdpOutAlignment
    // and wNdpOutPayloadRemainder every device we have seen reports.
    seq_ = static_cast<uint16_t>(seq_ + 1);
    const auto block_len = static_cast<uint16_t>(kTxDatagramOffset + len);

    std::vector<uint8_t> ntb;
    ntb.reserve(kTxDatagramOffset + len + 1);

    // NTH16.
    put_le32(ntb, kNth16Signature);
    put_le16(ntb, static_cast<uint16_t>(kNth16Length));
    put_le16(ntb, seq_);
    put_le16(ntb, block_len);
    put_le16(ntb, static_cast<uint16_t>(kNth16Length));  // wNdpIndex: NDP follows the NTH

    // NDP16: header, one datagram entry, terminator.
    put_le32(ntb, kNdp16Signature);
    put_le16(ntb, static_cast<uint16_t>(kNdp16Length));
    put_le16(ntb, 0);  // wNextNdpIndex: no chain
    put_le16(ntb, static_cast<uint16_t>(kTxDatagramOffset));
    put_le16(ntb, static_cast<uint16_t>(len));
    put_le16(ntb, 0);  // terminator index
    put_le16(ntb, 0);  // terminator length

    ntb.insert(ntb.end(), frame, frame + len);

    // A block that is an exact multiple of the bulk max packet size would need
    // a zero-length packet to terminate the transfer; pad instead.
    if (ntb.size() % 512 == 0)
    {
        ntb.push_back(0);
    }
    if (ntb.size() > out_max_)
    {
        SPDLOG_WARN("[ncm] tx NTB is {} bytes, over the device's dwNtbOutMaxSize {}", ntb.size(),
                    out_max_);
    }
    SPDLOG_DEBUG("[ncm] tx NTB seq={} blockLen={} datagrams=1 frame={} wire={}", seq_, block_len,
                 len, ntb.size());
    return ntb;
}

// ---------------- pumps ----------------

// Drains the control interface's interrupt endpoint. CDC devices announce link
// state there (NETWORK_CONNECTION, CONNECTION_SPEED_CHANGE) and the kernel's
// cdc_ncm always keeps a URB queued on it. We previously never read it at all.
void NcmBridge::statusLoop()
{
    constexpr size_t kNotificationSize = 64;
    constexpr unsigned kPollTimeoutMs = 1000;

    while (run_.load())
    {
        try
        {
            const auto notification = usbBulkIn(handle_, ep_int_, kNotificationSize, kPollTimeoutMs);
            if (notification.size() >= 8)
            {
                const uint8_t request = notification[1];
                const uint16_t value = get_le16(notification.data() + 2);
                switch (request)
                {
                    case 0x00:  // NETWORK_CONNECTION
                        SPDLOG_INFO("[ncm] link {} (NETWORK_CONNECTION)",
                                    value != 0 ? "UP" : "DOWN");
                        break;
                    case 0x2A:  // CONNECTION_SPEED_CHANGE
                        SPDLOG_INFO("[ncm] CONNECTION_SPEED_CHANGE ({} bytes)",
                                    notification.size());
                        break;
                    default:
                        SPDLOG_DEBUG("[ncm] notification 0x{:02x} ({} bytes)", request,
                                     notification.size());
                        break;
                }
            }
        }
        catch (const std::system_error& e)
        {
            const int err = e.code().value();
            if (err == ETIMEDOUT)
            {
                continue;  // no notification pending, normal
            }
            if (!run_.load() || err == ENODEV || err == ESHUTDOWN)
            {
                return;
            }
            SPDLOG_DEBUG("[ncm] status read error (errno {}): {}", err, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void NcmBridge::usbToTapLoop()
{
    // Bring-up switch: the two pumps issue synchronous libusb bulk transfers on
    // the same handle, so this exists to test whether the write path is being
    // starved by the read path. That question got sharper with libusb -- its
    // synchronous API serialises on an internal event lock, so one pump can end
    // up servicing the other's completions. The iAP2 session runs over the
    // carkit TLS channel rather than NCM, so it still comes up with this
    // disabled. See docs/carplay_bringup.md stage 6.
    if (std::getenv("CARPLAY_NCM_NO_READER") != nullptr)
    {
        SPDLOG_WARN("[ncm] CARPLAY_NCM_NO_READER set: not reading from USB");
        return;
    }

    while (run_.load())
    {
        std::vector<uint8_t> ntb;
        try
        {
            ntb = usbBulkIn(handle_, ep_in_, kUsbReadSize, kUsbReadTimeoutMs);
        }
        catch (const std::system_error& e)
        {
            if (!run_.load())
            {
                return;
            }
            const int err = e.code().value();
            if (err == ETIMEDOUT)
            {
                continue;
            }
            if (err == ENODEV || err == EIO || err == ESHUTDOWN || err == EPROTO)
            {
                SPDLOG_WARN("[ncm] usb->tap read ended (errno {}): {}", err, e.what());
                return;
            }
            SPDLOG_WARN("[ncm] usb->tap read error (errno {}): {}", err, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ntb.empty())
        {
            continue;
        }

        for (const auto& frame : parseNtb(ntb))
        {
            const ssize_t written = ::write(tap_fd_, frame.data(), frame.size());
            if (written < 0)
            {
                if (!run_.load())
                {
                    return;
                }
                SPDLOG_WARN("[ncm] write({} bytes) to {} failed: {}", frame.size(), ifname_,
                            strerror(errno));
            }
        }
    }
}

void NcmBridge::tapToUsbLoop()
{
    std::vector<uint8_t> frame(kTapReadSize);
    while (run_.load())
    {
        pollfd p{};
        p.fd = tap_fd_;
        p.events = POLLIN;
        const int n = ::poll(&p, 1, kTapPollTimeoutMs);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            SPDLOG_WARN("[ncm] poll({}) failed: {}", ifname_, strerror(errno));
            return;
        }
        if (n == 0)
        {
            continue;
        }

        const ssize_t len = ::read(tap_fd_, frame.data(), frame.size());
        if (len <= 0)
        {
            if (!run_.load())
            {
                return;
            }
            if (len < 0 && errno != EAGAIN && errno != EINTR)
            {
                SPDLOG_WARN("[ncm] read({}) failed: {}", ifname_, strerror(errno));
            }
            continue;
        }

        try
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            const std::vector<uint8_t> ntb = buildNtb(frame.data(), static_cast<size_t>(len));
            usbBulkOut(handle_, ep_out_, ntb.data(), ntb.size(), kUsbWriteTimeoutMs);
        }
        catch (const std::system_error& e)
        {
            if (!run_.load())
            {
                return;
            }
            const int err = e.code().value();
            if (err == ENODEV || err == EIO || err == ESHUTDOWN || err == EPROTO)
            {
                SPDLOG_WARN("[ncm] tap->usb write ended (errno {}): {}", err, e.what());
                return;
            }
            // The phone only powers up its NCM data path once the CarPlay
            // session is actually running, so writes issued before that -- the
            // kernel starts emitting router/neighbour solicitations the moment
            // the TAP has carrier -- time out. A timed-out URB is unlinked,
            // which can leave the endpoint's toggle out of step and wedge every
            // later write, so resynchronise before dropping the frame.
            if (err == ETIMEDOUT)
            {
                usbClearHalt(handle_, ep_out_);
            }
            SPDLOG_WARN("[ncm] tap->usb write error (errno {}): {}", err, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

}  // namespace apple_usb
