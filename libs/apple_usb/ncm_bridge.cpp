// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/ncm_bridge.py
#include "apple_usb/ncm_bridge.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/if_tun.h>
#include <linux/usbdevice_fs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// CDC class requests / descriptor sub-types.
constexpr uint8_t kCdcInterfaceClass = 0x02;
constexpr uint8_t kCdcNcmSubClass = 0x0d;
constexpr uint8_t kCdcDataInterfaceClass = 0x0a;
constexpr uint8_t kGetNtbParameters = 0x80;
constexpr uint8_t kDescriptorTypeInterface = 0x04;
constexpr uint8_t kDescriptorTypeCsInterface = 0x24;
constexpr uint8_t kCdcEthernetFunctionalDescriptor = 0x0f;

// The NCM data interface carries its bulk endpoints on altsetting 1;
// altsetting 0 is the mandatory "no data" setting.
constexpr unsigned kNcmDataAltSetting = 1;

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

// Parse a sysfs attribute holding a number in the given base. Returns
// `fallback` when the attribute is missing or unparseable.
uint32_t readSysfsUint(const fs::path& dir, const char* attr, int base, uint32_t fallback = 0)
{
    const std::string text = readSysfsAttr(dir, attr);
    if (text.empty())
    {
        return fallback;
    }
    uint32_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    if (std::from_chars(first, last, value, base).ec != std::errc{})
    {
        return fallback;
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

// TAP devices are named cpusb0, cpusb1, ... across all bridges in a process.
std::atomic<unsigned> g_tap_seq{0};

// Run a command, logging the exact argv and any output. Returns the exit
// status, or -1 when the child could not be started / was killed.
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

bool NcmBridge::kernelNcmBound() const
{
    std::error_code ec;
    const fs::path root = fs::canonical(device_.sysfs_path, ec);
    if (ec)
    {
        return false;
    }
    for (const auto& entry : fs::directory_iterator("/sys/class/net", ec))
    {
        const fs::path base = entry.path() / "device";
        std::error_code link_ec;
        const fs::path driver = fs::canonical(base / "driver", link_ec);
        if (link_ec || driver.filename() != "cdc_ncm")
        {
            continue;
        }
        const fs::path dev = fs::canonical(base, link_ec);
        if (link_ec)
        {
            continue;
        }
        if (dev.string().starts_with(root.string() + "/"))
        {
            SPDLOG_INFO("[ncm] kernel cdc_ncm already owns {} under {}",
                        entry.path().filename().string(), root.string());
            return true;
        }
    }
    return false;
}

bool NcmBridge::findNcmPair(unsigned& ctrl_if, unsigned& data_if) const
{
    std::error_code ec;
    const fs::path root = fs::canonical(device_.sysfs_path, ec);
    if (ec)
    {
        SPDLOG_ERROR("[ncm] cannot resolve {}: {}", device_.sysfs_path, ec.message());
        return false;
    }
    const std::string dev = root.filename().string();
    const std::string cfg = readSysfsAttr(root, "bConfigurationValue");
    const std::string prefix = dev + ":" + cfg + ".";

    // Sort so the *first* eligible pair wins deterministically, matching the
    // Python's sorted(os.listdir(...)).
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(root, ec))
    {
        if (entry.path().filename().string().starts_with(prefix))
        {
            entries.push_back(entry.path());
        }
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& ipath : entries)
    {
        if (readSysfsAttr(ipath, "bInterfaceClass") != "02")
        {
            continue;
        }
        if (readSysfsAttr(ipath, "bInterfaceSubClass") != "0d")
        {
            continue;
        }
        std::error_code drv_ec;
        if (fs::is_symlink(ipath / "driver", drv_ec))
        {
            SPDLOG_DEBUG("[ncm] {} already has a driver bound; skipping",
                         ipath.filename().string());
            continue;
        }
        // sysfs reports bInterfaceNumber in hex.
        const unsigned ctrl = readSysfsUint(ipath, "bInterfaceNumber", 16);
        const fs::path data_path = root / fmt::format("{}:{}.{}", dev, cfg, ctrl + 1);
        const std::string data_class = readSysfsAttr(data_path, "bInterfaceClass");
        if (data_class != "0a")
        {
            SPDLOG_DEBUG("[ncm] control iface {} has no CDC-data successor (class='{}')", ctrl,
                         data_class);
            continue;
        }
        ctrl_if = ctrl;
        data_if = ctrl + 1;
        SPDLOG_INFO("[ncm] NCM pair on {}: control iface {} (class 0x{:02x}/sub 0x{:02x}), data "
                    "iface {} (class 0x{:02x})",
                    device_.serial, ctrl_if, kCdcInterfaceClass, kCdcNcmSubClass, data_if,
                    kCdcDataInterfaceClass);
        return true;
    }
    SPDLOG_ERROR("[ncm] no unbound NCM pair on {} (config {})", device_.serial, cfg);
    return false;
}

std::string NcmBridge::parseHostMac(unsigned ctrl_if) const
{
    const fs::path descriptors = fs::path(device_.sysfs_path) / "descriptors";
    std::ifstream in(descriptors, std::ios::binary);
    if (!in)
    {
        SPDLOG_WARN("[ncm] cannot read {}", descriptors.string());
        return {};
    }
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());

    // Walk the configuration descriptor blob, tracking which interface the
    // class-specific descriptors belong to, and pick up the Ethernet
    // Networking functional descriptor's iMACAddress string index.
    size_t idx = 0;
    int cur_if = -1;
    uint8_t mac_str_idx = 0;
    while (idx + 2 <= raw.size())
    {
        const uint8_t blen = raw[idx];
        const uint8_t btype = raw[idx + 1];
        if (blen < 2)
        {
            SPDLOG_WARN("[ncm] descriptor blob: zero-length descriptor at offset {}", idx);
            break;
        }
        if (btype == kDescriptorTypeInterface && blen >= 3)
        {
            cur_if = raw[idx + 2];
        }
        else if (btype == kDescriptorTypeCsInterface && blen >= 4 &&
                 cur_if == static_cast<int>(ctrl_if) &&
                 raw[idx + 2] == kCdcEthernetFunctionalDescriptor)
        {
            mac_str_idx = raw[idx + 3];
            break;
        }
        idx += blen;
    }
    if (mac_str_idx == 0)
    {
        SPDLOG_WARN("[ncm] no CDC Ethernet functional descriptor for iface {}", ctrl_if);
        return {};
    }

    std::vector<uint8_t> d;
    try
    {
        // GET_DESCRIPTOR(STRING, mac_str_idx), langid 0x0409.
        d = usbControl(fd_, 0x80, 6, static_cast<uint16_t>((3u << 8) | mac_str_idx), 0x0409, 64);
    }
    catch (const std::system_error& e)
    {
        SPDLOG_WARN("[ncm] GET_DESCRIPTOR(string {}) failed: {}", mac_str_idx, e.what());
        return {};
    }
    if (d.size() < 2 || d[1] != 3)
    {
        SPDLOG_WARN("[ncm] string descriptor {} malformed (len={}, type={})", mac_str_idx, d.size(),
                    d.empty() ? 0 : d[1]);
        return {};
    }
    // UTF-16LE, 12 hex characters. Take the low byte of each code unit.
    const size_t end = std::min<size_t>(d[0], d.size());
    std::string hex;
    for (size_t i = 2; i + 1 < end; i += 2)
    {
        if (d[i + 1] != 0)
        {
            continue;
        }
        hex.push_back(static_cast<char>(d[i]));
    }
    if (hex.size() != 12)
    {
        SPDLOG_WARN("[ncm] iMACAddress string is {} chars, expected 12 ('{}')", hex.size(), hex);
        return {};
    }
    std::string mac;
    for (size_t i = 0; i < 12; i += 2)
    {
        if (!mac.empty())
        {
            mac.push_back(':');
        }
        mac.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i]))));
        mac.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i + 1]))));
    }
    return mac;
}

bool NcmBridge::findBulkEndpoints(unsigned data_if, uint8_t& ep_in, uint8_t& ep_out) const
{
    std::error_code ec;
    const fs::path root = fs::canonical(device_.sysfs_path, ec);
    if (ec)
    {
        return false;
    }
    const std::string dev = root.filename().string();
    const std::string cfg = readSysfsAttr(root, "bConfigurationValue");
    const fs::path ipath = root / fmt::format("{}:{}.{}", dev, cfg, data_if);

    ep_in = 0;
    ep_out = 0;
    for (const auto& entry : fs::directory_iterator(ipath, ec))
    {
        if (!entry.path().filename().string().starts_with("ep_"))
        {
            continue;
        }
        if (readSysfsAttr(entry.path(), "type") != "Bulk")
        {
            continue;
        }
        const auto addr = static_cast<uint8_t>(readSysfsUint(entry.path(), "bEndpointAddress", 16));
        if (addr & 0x80)
        {
            ep_in = addr;
        }
        else
        {
            ep_out = addr;
        }
    }
    if (ec)
    {
        SPDLOG_ERROR("[ncm] cannot list endpoints under {}: {}", ipath.string(), ec.message());
        return false;
    }
    if (ep_in == 0 || ep_out == 0)
    {
        SPDLOG_ERROR("[ncm] bulk endpoints not found under {} (in=0x{:02x} out=0x{:02x}); is "
                     "altsetting {} active?",
                     ipath.string(), ep_in, ep_out, kNcmDataAltSetting);
        return false;
    }
    return true;
}

// ---------------- setup ----------------

bool NcmBridge::createTap()
{
    ifname_ = fmt::format("cpusb{}", g_tap_seq.fetch_add(1));

    tap_fd_ = ::open("/dev/net/tun", O_RDWR);
    if (tap_fd_ < 0)
    {
        SPDLOG_ERROR("[ncm] open(/dev/net/tun) failed: {}", strerror(errno));
        return false;
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname_.c_str(), IFNAMSIZ - 1);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (::ioctl(tap_fd_, TUNSETIFF, &ifr) < 0)
    {
        SPDLOG_ERROR("[ncm] TUNSETIFF({}) failed: {}", ifname_, strerror(errno));
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
        // talk to us if we use the random one the kernel generated.
        runCommand({"ip", "link", "set", ifname_, "address", mac});
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
            SPDLOG_WARN("[ncm] cannot write {} (DAD stays enabled)", dad_path);
        }
    }

    runCommand({"ip", "link", "set", ifname_, "up"});

    // Derive the accessory link-local from whatever MAC the interface actually
    // ended up with, so a failed "ip link set address" cannot desync the two.
    const std::string actual_mac =
        readSysfsAttr(fs::path("/sys/class/net") / ifname_, "address");
    fe80_ = deriveEui64LinkLocal(actual_mac);
    if (fe80_.empty())
    {
        SPDLOG_ERROR("[ncm] cannot derive fe80 from {} MAC '{}'", ifname_, actual_mac);
        return false;
    }
    SPDLOG_INFO("[ncm] {} mac={} -> link-local {}", ifname_, actual_mac, fe80_);
    runCommand({"ip", "-6", "addr", "add", fe80_ + "/64", "dev", ifname_});
    return true;
}

bool NcmBridge::start()
{
    if (run_.load())
    {
        SPDLOG_WARN("[ncm] start() called while already running on {}", ifname_);
        return true;
    }
    if (device_.sysfs_path.empty())
    {
        SPDLOG_ERROR("[ncm] device has no sysfs path");
        return false;
    }
    if (kernelNcmBound())
    {
        SPDLOG_ERROR("[ncm] refusing to claim: kernel cdc_ncm is bound to {}", device_.serial);
        return false;
    }
    if (!findNcmPair(ctrl_iface_, data_iface_))
    {
        return false;
    }

    const auto fd = openDevice(device_);
    if (!fd)
    {
        return false;
    }
    fd_ = *fd;

    try
    {
        usbClaimInterface(fd_, ctrl_iface_);
        ctrl_claimed_ = true;
        usbClaimInterface(fd_, data_iface_);
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
        const auto params = usbControl(fd_, 0xA1, kGetNtbParameters, 0,
                                       static_cast<uint16_t>(ctrl_iface_), 28);
        if (params.size() >= 28)
        {
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

    const std::string mac = parseHostMac(ctrl_iface_);

    // Activate the data altsetting; the bulk endpoints only exist there.
    usbdevfs_setinterface setif{};
    setif.interface = data_iface_;
    setif.altsetting = kNcmDataAltSetting;
    if (::ioctl(fd_, USBDEVFS_SETINTERFACE, &setif) < 0)
    {
        SPDLOG_ERROR("[ncm] USBDEVFS_SETINTERFACE(iface {}, alt {}) failed: {}", data_iface_,
                     kNcmDataAltSetting, strerror(errno));
        cleanup();
        return false;
    }

    if (!findBulkEndpoints(data_iface_, ep_in_, ep_out_))
    {
        cleanup();
        return false;
    }

    if (!createTap() || !configureInterface(mac))
    {
        cleanup();
        return false;
    }

    run_.store(true);
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
    if (fd_ >= 0)
    {
        const auto release = [this](unsigned iface)
        {
            if (::ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &iface) < 0)
            {
                SPDLOG_DEBUG("[ncm] USBDEVFS_RELEASEINTERFACE({}) failed: {}", iface,
                             strerror(errno));
            }
        };
        if (data_claimed_)
        {
            release(data_iface_);
            data_claimed_ = false;
        }
        if (ctrl_claimed_)
        {
            release(ctrl_iface_);
            ctrl_claimed_ = false;
        }
        ::close(fd_);
        fd_ = -1;
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

void NcmBridge::usbToTapLoop()
{
    while (run_.load())
    {
        std::vector<uint8_t> ntb;
        try
        {
            ntb = usbBulkIn(fd_, ep_in_, kUsbReadSize, kUsbReadTimeoutMs);
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
            usbBulkOut(fd_, ep_out_, ntb.data(), ntb.size(), kUsbWriteTimeoutMs);
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
            SPDLOG_WARN("[ncm] tap->usb write error (errno {}): {}", err, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

}  // namespace apple_usb
