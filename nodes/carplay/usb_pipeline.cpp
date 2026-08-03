// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_pipeline.h"

#include "iap2_session.h"

#include "apple_usb/lockdown.h"
#include "apple_usb/muxd.h"
#include "apple_usb/ncm_discovery.h"

#include "airplay/receiver.h"
#include "iap2/mcp2221a_mfi_signer.h"
#include "apple_usb/usb_device.h"
#include "apple_usb/usbmuxd_server.h"

#include <spdlog/spdlog.h>

// Whether the operating system already provides a usbmux daemon we can use.
// macOS does; Linux's will not switch the phone to configuration 6, so stage 3
// brings its own.
//
// Defaults to the host but can be overridden, so either branch can be
// type-checked from either platform. That is not hypothetical tidiness: the
// Linux branch is the one that cannot be built on a developer's Mac otherwise,
// and it is also the one that matters in the car. Compile it with
// -DCARPLAY_USE_SYSTEM_MUX=0.
//
// There was a matching CARPLAY_USE_SYSTEM_NCM once, because stage 6 used to
// have two implementations: look the interface up, or build the link ourselves
// over usbfs into a TAP. Only the lookup survives -- both operating systems
// bring the link up on their own -- so there is nothing left to select between.
#if !defined(CARPLAY_USE_SYSTEM_MUX)
#  if defined(__APPLE__)
#    define CARPLAY_USE_SYSTEM_MUX 1
#  else
#    define CARPLAY_USE_SYSTEM_MUX 0
#  endif
#endif

// Stage 6 is interface lookup: the system NCM driver already built the link, so
// all this needs is a way to match an interface by MAC and read its link-local.
// macOS wants the BSD link-layer sockaddr for the first half; Linux publishes
// the same thing as text in sysfs.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#else
#include <fstream>
#endif

#include <cctype>
#include <cerrno>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace fs = std::filesystem;

namespace carplay
{
namespace
{

constexpr auto kRediscoverPoll = std::chrono::milliseconds(200);
constexpr auto kRediscoverTimeout = std::chrono::seconds(15);

// How often to look for a phone appearing, and how long to pause after a
// session ends before trying again -- long enough that a phone that fails
// repeatedly does not spin the log, short enough to feel immediate on a replug.
constexpr auto kDevicePoll = std::chrono::milliseconds(500);
constexpr auto kReattachDelay = std::chrono::seconds(2);
constexpr auto kMaxReattachDelay = std::chrono::seconds(30);

std::string shortUdid(const std::string& udid)
{
    return udid.size() > 8 ? udid.substr(0, 8) : udid;
}

VideoCodec toBridgeCodec(airplay::nalu::Codec codec)
{
    return codec == airplay::nalu::Codec::H265 ? VideoCodec::H265 : VideoCodec::H264;
}

// One rotary-controller event from the dashboard onto the knob HID device.
// Buttons are sent as clicks (press then release), which is what a head unit's
// momentary switch is; rotation and pan are relative and need no release.
void sendKnobEvent(airplay::Receiver& receiver, const InputEvent& event)
{
    airplay::hid::KnobState state;
    switch (static_cast<InputEvent::KnobControl>(event.code))
    {
        case InputEvent::KnobControl::Select:
            state.select = event.value != 0;
            break;
        case InputEvent::KnobControl::Home:
            state.home = event.value != 0;
            break;
        case InputEvent::KnobControl::Back:
            state.back = event.value != 0;
            break;
        case InputEvent::KnobControl::Rotate:
            state.wheel = event.value;
            break;
        case InputEvent::KnobControl::PanX:
            state.pan_x = event.value;
            break;
        case InputEvent::KnobControl::PanY:
            state.pan_y = event.value;
            break;
        default:
            SPDLOG_WARN("[node] ignoring unknown knob control {}", event.code);
            return;
    }
    receiver.sendKnob(state);
}

// The configuration switch re-enumerates the phone, which changes its device
// address. Everything downstream resolves the device afresh, so the DeviceInfo
// has to be re-read afterwards rather than reused.
//
// Tracked by physical port rather than by serial number. The phone does not
// move between sockets while it re-enumerates, so the port path is stable
// across exactly the event that invalidates everything else -- and unlike the
// serial, reading it needs no device open, so the detection poll below stays
// free.
std::optional<apple_usb::DeviceInfo> rediscover(const apple_usb::PortPath& port)
{
    const auto deadline = std::chrono::steady_clock::now() + kRediscoverTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto device = apple_usb::findDeviceAt(port); device)
        {
            return device;
        }
        std::this_thread::sleep_for(kRediscoverPoll);
    }
    return std::nullopt;
}

// Enumeration leaves DeviceInfo::serial empty because filling it in costs a
// device open. Do it once, here.
//
// Deliberately called *before* the CarPlay configuration switch, while the
// phone is still in the configuration it boots into: the UDID is what names
// the pair record and the usbmuxd socket, and having it in hand before the
// re-enumeration means a phone that fails to come back can still be named in
// the error. The serial is then carried across the switch by the caller --
// it is the same physical device on the same port.
bool populateSerial(apple_usb::DeviceInfo& device)
{
    apple_usb::DeviceHandle handle = apple_usb::openDevice(device);
    if (!handle)
    {
        SPDLOG_ERROR("[usb] cannot open the device at port {} to read its UDID. On Linux this "
                     "is a permissions problem -- install nodes/carplay/udev/99-carplay.rules "
                     "or run as root.", device.port.toString());
        return false;
    }
    device.serial = apple_usb::readSerial(handle);
    if (device.serial.empty())
    {
        SPDLOG_ERROR("[usb] the device at port {} reports no UDID", device.port.toString());
        return false;
    }
    return true;
}

// Pair records live here, so this has to outlive a reboot: XDG_RUNTIME_DIR is
// tmpfs and gets wiped, which silently re-prompts for trust on the phone at
// every boot. The mux socket is created here too, which is fine -- it is
// unlinked on the way in and out.
std::string defaultStateDir()
{
    if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr)
    {
        return (fs::path(data_home) / "carplay").string();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr)
    {
        return (fs::path(home) / ".local" / "share" / "carplay").string();
    }
    return (fs::temp_directory_path() / "carplay").string();
}

// --- Stage 2: device detection and the CarPlay configuration switch ---------
//
// Blocks until an Apple device shows up, or `stop` is set. Polling sysfs rather
// than subscribing to udev keeps this working unprivileged and inside a
// container, and half a second of latency on a plug event is imperceptible.
std::optional<apple_usb::DeviceInfo> waitForDevice(std::atomic<bool>& stop)
{
    bool announced = false;
    while (!stop.load())
    {
        auto devices = apple_usb::listAppleDevices();
        if (!devices.empty())
        {
            for (const auto& device : devices)
            {
                SPDLOG_INFO("[usb] found {:04x}:{:04x} at port {} (config {} of {})", device.vid,
                            device.pid, device.port.toString(), device.active_configuration,
                            device.num_configurations);
            }
            if (devices.size() > 1)
            {
                SPDLOG_WARN("[usb] {} Apple devices present; using the one at port {}",
                            devices.size(), devices.front().port.toString());
            }

            // The UDID is not part of enumeration any more; read it now, once.
            // A device we cannot open is not usable, so keep waiting rather
            // than failing the whole bring-up on a transient permission or
            // settling problem.
            if (!populateSerial(devices.front()))
            {
                std::this_thread::sleep_for(kDevicePoll);
                continue;
            }
            SPDLOG_INFO("[usb] udid={} at port {}", shortUdid(devices.front().serial),
                        devices.front().port.toString());
            return devices.front();
        }

        if (!announced)
        {
            announced = true;
            SPDLOG_INFO("[usb] waiting for a phone. Plug one in, unlocked; on a VM, confirm "
                        "it is attached to the guest.");
        }
        std::this_thread::sleep_for(kDevicePoll);
    }
    return std::nullopt;
}

// Puts an already-detected phone into the CarPlay configuration, re-reading it
// afterwards because the switch re-enumerates it.
std::optional<apple_usb::DeviceInfo> switchToCarPlay(apple_usb::DeviceInfo device)
{
    if (device.active_configuration == apple_usb::kCarPlayConfiguration)
    {
        SPDLOG_INFO("[usb] already in configuration {}", apple_usb::kCarPlayConfiguration);
        return device;
    }

    SPDLOG_INFO("[usb] switching udid={} from configuration {} to {}",
                shortUdid(device.serial), device.active_configuration,
                apple_usb::kCarPlayConfiguration);

    if (!apple_usb::switchToCarPlayConfiguration(device))
    {
        SPDLOG_ERROR("[usb] configuration switch failed for udid={}", shortUdid(device.serial));
        return std::nullopt;
    }

    auto refreshed = rediscover(device.port);
    if (!refreshed)
    {
        SPDLOG_ERROR("[usb] udid={} did not come back at port {} after the configuration "
                     "switch. On a VM this usually means the hypervisor handed the "
                     "re-enumerating device back to the host.", shortUdid(device.serial),
                     device.port.toString());
        return std::nullopt;
    }

    // Same socket, same phone: carry the UDID across rather than paying for
    // another open to re-read a string descriptor that cannot have changed.
    refreshed->serial = device.serial;

    if (refreshed->active_configuration != apple_usb::kCarPlayConfiguration)
    {
        SPDLOG_ERROR("[usb] udid={} is in configuration {}, expected {}. Something "
                     "re-enumerated it -- check that the system usbmuxd is stopped.",
                     shortUdid(refreshed->serial), refreshed->active_configuration,
                     apple_usb::kCarPlayConfiguration);
        return std::nullopt;
    }

    SPDLOG_INFO("[usb] udid={} now in configuration {} at port {}", shortUdid(refreshed->serial),
                refreshed->active_configuration, refreshed->port.toString());
    return refreshed;
}

// Everything the stages below need that outlives an individual phone: the MFi
// coprocessor sits on I2C and is unaffected by a phone coming and going, so it
// is set up once and lent to each session.
struct SessionContext
{
    const NodeConfig& options;
    std::string state_dir;
    iap2::MfiSigner* mfi_signer = nullptr;
    std::shared_ptr<std::mutex> mfi_mutex;
    // Set true while an AirPlay session is live, so the caller's idle
    // session-state publisher stands down. Optional.
    std::atomic<bool>* recording = nullptr;
};

// What provides the usbmuxd socket that the lockdown and carkit layers connect
// to. The two platforms answer this very differently, and the difference is not
// cosmetic:
//
//  * Linux -- nothing else can do it for us. The distro's usbmuxd will not put
//    the phone into configuration 6, and it has to be stopped anyway or it
//    fights us for If1. So we drive the mux interface ourselves (MuxHost) and
//    serve the usbmuxd protocol on a private socket (UsbmuxdServer).
//
//  * macOS -- the system usbmuxd already owns If1, through an IOKit user
//    client rather than a kext, and it keeps muxing the phone after the switch
//    to configuration 6. Taking If1 from it would mean capturing the whole
//    device, and capture is all-or-nothing: it would also strip the NCM
//    interfaces from AppleUSBNCM, which is precisely what stage 6 wants left
//    alone. Stopping the daemon is not an option either -- launchctl bootout is
//    refused while SIP is engaged. So we connect to its socket and let it work.
//
//    Verified on hardware 2026-08-01 with the phone in configuration 6: our own
//    UsbmuxClient drives Apple's daemon for ReadBUID, ListDevices,
//    ReadPairRecord and Connect-to-lockdown, unprivileged. The pair record is
//    already there, because the Mac is already trusted by the phone -- so macOS
//    skips the trust prompt that Linux has to wait for.
class SessionMux
{
  public:
    bool open(const apple_usb::DeviceInfo& device, const std::string& state_dir);
    void close();

    // False once the phone has gone, which unwinds the session.
    bool alive() const;

    const std::string& socketPath() const { return socket_path_; }

  private:
    std::string socket_path_;
#if CARPLAY_USE_SYSTEM_MUX
    apple_usb::PortPath port_;
#else
    // unique_ptr rather than by value: UsbmuxdServer holds a MuxHost& and both
    // have to stay put for the life of the session.
    std::unique_ptr<apple_usb::MuxHost> mux_;
    std::unique_ptr<apple_usb::UsbmuxdServer> server_;
#endif
};

#if CARPLAY_USE_SYSTEM_MUX

// The system daemon's socket. World read/write, so this needs no privilege --
// the only thing on macOS that does is the configuration switch itself.
constexpr const char* kSystemUsbmuxdSocket = "/var/run/usbmuxd";

bool SessionMux::open(const apple_usb::DeviceInfo& device, const std::string&)
{
    port_ = device.port;
    socket_path_ = kSystemUsbmuxdSocket;

    std::error_code ec;
    if (!fs::exists(socket_path_, ec))
    {
        SPDLOG_ERROR("[muxd] {} is not there, so the system usbmuxd is not running. It is "
                     "part of MobileDevice.framework and normally always up; without it "
                     "there is no way to reach the phone's lockdown port on macOS.",
                     socket_path_);
        return false;
    }

    SPDLOG_INFO("[muxd] using the system usbmuxd at {} for udid={}", socket_path_,
                shortUdid(device.serial));
    return true;
}

void SessionMux::close() {}

bool SessionMux::alive() const
{
    // Nothing of ours is attached to the device, so liveness is simply whether
    // it is still plugged into the same port.
    return apple_usb::findDeviceAt(port_).has_value();
}

#else

bool SessionMux::open(const apple_usb::DeviceInfo& device, const std::string& state_dir)
{
    mux_ = std::make_unique<apple_usb::MuxHost>(device);
    if (!mux_->open())
    {
        SPDLOG_ERROR("[muxd] could not open the mux for udid={}. Another driver may hold "
                     "the interface -- check `lsusb -t` and that the system usbmuxd is "
                     "stopped.", shortUdid(device.serial));
        mux_.reset();
        return false;
    }
    SPDLOG_INFO("[muxd] mux open for udid={}", shortUdid(device.serial));

    std::error_code ec;
    socket_path_ =
        (fs::path(state_dir) / ("usbmuxd-" + shortUdid(device.serial) + ".sock")).string();
    fs::remove(socket_path_, ec);

    server_ = std::make_unique<apple_usb::UsbmuxdServer>(*mux_, socket_path_, state_dir);
    if (!server_->start())
    {
        SPDLOG_ERROR("[usbmuxd] could not serve on {}", socket_path_);
        server_.reset();
        mux_->close();
        mux_.reset();
        return false;
    }
    SPDLOG_INFO("[usbmuxd] serving {} on {}", shortUdid(device.serial), socket_path_);
    SPDLOG_INFO("[usbmuxd] sanity-check it independently with:");
    SPDLOG_INFO("[usbmuxd]   USBMUXD_SOCKET_ADDRESS=UNIX:{} idevice_id -l", socket_path_);
    return true;
}

void SessionMux::close()
{
    if (server_)
    {
        server_->stop();
        server_.reset();
    }
    if (mux_)
    {
        mux_->close();
        mux_.reset();
    }
    if (!socket_path_.empty())
    {
        std::error_code ec;
        fs::remove(socket_path_, ec);
    }
}

bool SessionMux::alive() const
{
    return mux_ && mux_->alive();
}

#endif

// The accessory-side endpoint the phone is told to dial after authentication:
// an interface carrying an IPv6 link-local, on the same ethernet segment as the
// phone's first NCM function.
//
// The system NCM driver has already built this link by the time we look --
// AppleUSBNCM on macOS, cdc_ncm on Linux -- binding the NCM function as soon as
// configuration 6 is applied and giving it a real network interface. Neither
// needs anything driven and neither needs privilege, so there is no bridge
// here: the only question is *which* interface belongs to the first NCM
// function, and iMACAddress answers it.
//
// Verified on hardware 2026-08-01 (macOS): en9 / ca:1f:e8:0f:24:b1 /
// fe80::1ca1:5cd0:be56:35c6 for the first function, en8 for the second. And
// 2026-08-02 (Linux): enxca1fe80f24b1 / fe80::c81f:e8ff:fe0f:24b1, same phone,
// same host MAC, 4713 frames over three minutes.
//
// Do not pick "the interface that just appeared" -- the iAP interface brings up
// one too (AppleUSBEthernetHost / ipheth), and it is not this.
//
// What the system does *not* do is address the interface: it has to be up with
// a link-local before this succeeds. macOS does that itself; on Linux it takes
// a one-line network profile, which is what nodes/carplay/udev/*.network and
// *.nmconnection are.
class AvLink
{
  public:
    bool start(const apple_usb::DeviceInfo& device);
    void stop();

    bool running() const { return running_; }
    const std::string& interfaceName() const { return ifname_; }

    // The link-local with its scope attached ("fe80::1%en9"), which is the only
    // form that can actually be bound: a link-local without its interface is
    // ambiguous. Empty when the link is not up.
    std::string scopedLinkLocal() const
    {
        if (link_local_.empty() || ifname_.empty()) return {};
        return link_local_ + "%" + ifname_;
    }

    const std::string& hostMac() const { return host_mac_; }
    const std::string& linkLocalAddress() const { return link_local_; }

  private:
    bool running_ = false;
    std::string ifname_;
    std::string host_mac_;
    std::string link_local_;
};

// Normalise a MAC to lowercase colon-separated form. The CDC iMACAddress string
// descriptor is 12 hex characters with no separators ("CA1FE80F24B1"), while
// getifaddrs hands back bytes -- so both sides get funnelled through this
// rather than compared as text in whatever shape they arrived in.
std::string normaliseMac(const std::string& text)
{
    std::string hex;
    for (const char c : text)
    {
        if (std::isxdigit(static_cast<unsigned char>(c)))
        {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (hex.size() != 12)
    {
        return {};
    }
    std::string out;
    for (size_t i = 0; i < 12; i += 2)
    {
        if (!out.empty())
        {
            out.push_back(':');
        }
        out += hex.substr(i, 2);
    }
    return out;
}

#if !defined(__APPLE__)

// The name of the interface whose link-layer address is `mac`, or "".
//
// Linux publishes every interface's MAC as text under /sys/class/net, so this
// needs no getifaddrs and no link-layer sockaddr at all -- the same lookup the
// BSD version below spells out in twenty lines.
std::string interfaceWithMac(const std::string& mac)
{
    const std::string wanted = normaliseMac(mac);
    if (wanted.empty())
    {
        return {};
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/net", ec))
    {
        std::ifstream in(entry.path() / "address");
        std::string text;
        if (!(in >> text))
        {
            continue;
        }
        if (normaliseMac(text) == wanted)
        {
            return entry.path().filename().string();
        }
    }
    return {};
}

#else

// The BSD name of the interface whose link-layer address is `mac`, or "".
std::string interfaceWithMac(const std::string& mac)
{
    const std::string wanted = normaliseMac(mac);
    if (wanted.empty())
    {
        return {};
    }

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        SPDLOG_WARN("[ncm] getifaddrs failed: {}", std::strerror(errno));
        return {};
    }

    std::string found;
    for (const ifaddrs* it = list; it != nullptr && found.empty(); it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_LINK)
        {
            continue;
        }
        const auto* dl = reinterpret_cast<const sockaddr_dl*>(it->ifa_addr);
        if (dl->sdl_alen != 6)
        {
            continue;
        }
        const auto* bytes = reinterpret_cast<const unsigned char*>(LLADDR(dl));
        const std::string candidate =
            fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", bytes[0], bytes[1], bytes[2],
                        bytes[3], bytes[4], bytes[5]);
        if (candidate == wanted && it->ifa_name != nullptr)
        {
            found = it->ifa_name;
        }
    }
    ::freeifaddrs(list);
    return found;
}

#endif

#if !defined(__APPLE__)

// The first *bindable* IPv6 link-local on `ifname`, or "".
//
// Read from /proc/net/if_inet6 rather than getifaddrs, because that is the only
// place an address's flags are visible. An address still running duplicate
// address detection is reported by getifaddrs exactly like a finished one, but
// bind() rejects it with EADDRNOTAVAIL -- so taking the first address we see
// races DAD and fails intermittently on a fresh plug, which is precisely what
// happened before this was written: the interface came up, the address was
// there, and the AirPlay listener still could not take it.
//
// Columns are: address (32 hex chars), interface index, prefix length, scope,
// flags, name -- all hex, all without separators.
std::string linkLocalOf(const std::string& ifname)
{
    std::ifstream in("/proc/net/if_inet6");
    std::string hex, index, prefix_len, scope, flags, name;
    while (in >> hex >> index >> prefix_len >> scope >> flags >> name)
    {
        if (name != ifname || hex.size() != 32)
        {
            continue;
        }
        // 0x20 is the link-local scope; a global address on this interface is
        // not what the phone is told to dial.
        if (std::strtoul(scope.c_str(), nullptr, 16) != 0x20u)
        {
            continue;
        }
        const unsigned long address_flags = std::strtoul(flags.c_str(), nullptr, 16);
        // IFA_F_TENTATIVE: DAD has not finished, so bind() would fail.
        // IFA_F_DADFAILED: someone else answered for it; it will never work.
        if ((address_flags & 0x40u) != 0 || (address_flags & 0x08u) != 0)
        {
            continue;
        }

        in6_addr address = {};
        for (size_t i = 0; i < 16; ++i)
        {
            address.s6_addr[i] =
                static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
        }
        char text[INET6_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET6, &address, text, sizeof(text)) != nullptr)
        {
            return text;
        }
    }
    return {};
}

#else

// The first IPv6 link-local on `ifname`, or "".
std::string linkLocalOf(const std::string& ifname)
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        return {};
    }

    std::string found;
    for (const ifaddrs* it = list; it != nullptr && found.empty(); it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET6)
        {
            continue;
        }
        if (it->ifa_name == nullptr || ifname != it->ifa_name)
        {
            continue;
        }
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(it->ifa_addr);
        in6_addr address = sin6->sin6_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&address))
        {
            continue;
        }
        // BSD may hide the scope id in bytes 2-3; clear it so what we advertise
        // is the address as everyone else writes it. See ncm_bridge.cpp.
        address.s6_addr[2] = 0;
        address.s6_addr[3] = 0;
        char text[INET6_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET6, &address, text, sizeof(text)) != nullptr)
        {
            found = text;
        }
    }
    ::freeifaddrs(list);
    return found;
}

#endif

bool AvLink::start(const apple_usb::DeviceInfo& device)
{
    const auto config = apple_usb::readActiveConfig(device);
    if (!config)
    {
        SPDLOG_ERROR("[ncm] cannot read the active configuration descriptor");
        return false;
    }

    const auto functions = apple_usb::findNcmFunctions(*config);
    if (functions.empty())
    {
        SPDLOG_ERROR("[ncm] no NCM function in configuration {}; is the phone really in the "
                     "CarPlay configuration?", config->value);
        return false;
    }
    // The first function, same choice the Linux bridge makes: its host MAC
    // shares an allocation with the phone's own address, the second's does not.
    const apple_usb::NcmFunction& fn = functions.front();
    SPDLOG_INFO("[ncm] NCM function control if{} data if{} (of {} function(s))", fn.ctrl_iface,
                fn.data_iface, functions.size());

    apple_usb::DeviceHandle handle = apple_usb::openDevice(device);
    if (!handle)
    {
        SPDLOG_ERROR("[ncm] cannot open the device to read iMACAddress");
        return false;
    }
    host_mac_ = normaliseMac(apple_usb::readStringDescriptor(handle, fn.mac_string_index));
    handle.reset();
    if (host_mac_.empty())
    {
        SPDLOG_ERROR("[ncm] iMACAddress (string descriptor {}) is unreadable, so the interface "
                     "AppleUSBNCM created cannot be identified", fn.mac_string_index);
        return false;
    }

    // The driver attaches a moment after the configuration switch, so give it
    // a little time rather than racing it on the first attachment.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        ifname_ = interfaceWithMac(host_mac_);
        if (!ifname_.empty())
        {
            link_local_ = linkLocalOf(ifname_);
        }
        if (!link_local_.empty())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (std::chrono::steady_clock::now() < deadline);

    if (ifname_.empty())
    {
        SPDLOG_ERROR("[ncm] no network interface has the host MAC {}. The system NCM driver "
                     "(AppleUSBNCM / cdc_ncm) should have created one when configuration 6 was "
                     "applied -- check `ip -br addr` or `ifconfig`, and that nothing captured "
                     "the device away from it.", host_mac_);
        return false;
    }
    if (link_local_.empty())
    {
        SPDLOG_ERROR("[ncm] {} exists but has no IPv6 link-local address, so there is nothing "
                     "to advertise to the phone.", ifname_);
        return false;
    }

    running_ = true;
    return true;
}

void AvLink::stop()
{
    // Nothing to tear down: the interface is the system's, not ours.
    running_ = false;
}

// Runs one attachment of one phone, from claiming the mux through to the iAP2
// session ending. Returns when the phone goes away or `stop` is set; the return
// value says whether the bring-up itself succeeded.
// --- Stage 7: the AirPlay RTSP receiver ------------------------------------
//
// Started before the iAP2 session, for the same reason the NCM bridge is: the
// phone dials port 7000 within milliseconds of CarPlayStartSession, and
// anything not listening by then just gets connection-refused.
//
// Returns the running receiver, or nullptr if it could not be started. The
// bridge callbacks installed here capture it, so the caller must detach them
// before letting it go.
std::unique_ptr<airplay::Receiver> startAirPlayReceiver(const SessionContext& ctx,
                                                        const AvLink& ncm, ZenohBridge& bridge)
{
    const NodeConfig& options = ctx.options;
    auto mfi_mutex = ctx.mfi_mutex;
    std::unique_ptr<airplay::Receiver> receiver;
        airplay::ReceiverConfig receiver_config;

        // Bind the AV link's address rather than the wildcard. On macOS the
        // system AirPlay Receiver (inside ControlCenter) already holds *:7000,
        // so the wildcard bind fails with EADDRINUSE -- but binding this one
        // address on the same port succeeds alongside it. The phone only ever
        // dials the link-local we advertised, so this loses nothing, and on
        // Linux it is the tighter bind anyway.
        //
        // Turning AirPlay Receiver off in System Settings would also free the
        // port, but requiring that of anyone running the node is worse than
        // simply not taking a port we were never entitled to.
        receiver_config.bind_address = ncm.scopedLinkLocal();

        receiver_config.name = options.vehicle.name;
        receiver_config.model = options.vehicle.model;
        receiver_config.manufacturer = options.vehicle.manufacturer;
        receiver_config.right_hand_drive = options.vehicle.right_hand_drive;
        receiver_config.device_id = options.device_id;
        // Same directory the lockdown pair records live in.
        receiver_config.state_dir = ctx.state_dir;
        receiver_config.width = options.display.width_px;
        receiver_config.height = options.display.height_px;
        receiver_config.fps = options.display.fps;
        receiver_config.physical_width_mm = options.display.physical_width_mm;
        receiver_config.primary_input = options.display.primary_input;
        receiver_config.allow_hevc = options.display.allow_hevc;
        receiver_config.oem_button = options.oem_button;
        if (receiver_config.oem_button.enabled && receiver_config.oem_button.icons.empty())
        {
            // The tile still appears, drawn with CarPlay's own placeholder --
            // which looks enough like a working button to hide a config mistake.
            SPDLOG_WARN("[node] manufacturer button is enabled but has no icon; pass "
                        "--config configs/carplay/carplay.yaml to supply the artwork");
        }

        if (ctx.mfi_signer != nullptr)
        {
            iap2::MfiSigner* signer = ctx.mfi_signer;
            receiver_config.mfi_certificate = [signer, mfi_mutex]() -> std::vector<uint8_t> {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->certificate().value_or(std::vector<uint8_t>{});
            };
            receiver_config.mfi_sign =
                [signer, mfi_mutex](const std::vector<uint8_t>& digest) -> std::vector<uint8_t> {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->signChallenge(digest).value_or(std::vector<uint8_t>{});
            };
            receiver_config.mfi_protocol_major = [signer, mfi_mutex]() {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->protocolMajor();
            };
        }
        receiver = std::make_unique<airplay::Receiver>(receiver_config);
        // Set before start(): the receiver pushes it to the phone at RECORD.
        receiver->setNightMode(options.night_mode);

        // Hand decoded access units straight to the dashboard. The parameter
        // sets are cached and re-sent ahead of every keyframe because zenoh has
        // no retained messages: a widget that starts late would otherwise never
        // sync.
        auto parameter_sets = std::make_shared<std::vector<uint8_t>>();
        receiver->setVideoHandler([&bridge, parameter_sets,
                                   &receiver_config](const airplay::VideoPacket& packet) {
            if (packet.is_config)
            {
                // Publish the parameter sets as their own message (the widget
                // caches them and prepends to the next access unit) and keep a
                // copy so we can republish before every keyframe -- zenoh has no
                // retained messages, so a widget that starts or restarts later
                // must see config again to sync.
                *parameter_sets = packet.data;

                VideoFrame config;
                config.codec = toBridgeCodec(packet.codec);
                config.is_config = true;
                config.width_px = static_cast<uint16_t>(receiver_config.width);
                config.height_px = static_cast<uint16_t>(receiver_config.height);
                config.data = packet.data.data();
                config.len = packet.data.size();
                bridge.publishVideo(config);
                return;
            }

            // Republish parameter sets immediately before each keyframe.
            if (packet.keyframe && !parameter_sets->empty())
            {
                VideoFrame config;
                config.codec = toBridgeCodec(packet.codec);
                config.is_config = true;
                config.width_px = static_cast<uint16_t>(receiver_config.width);
                config.height_px = static_cast<uint16_t>(receiver_config.height);
                config.data = parameter_sets->data();
                config.len = parameter_sets->size();
                bridge.publishVideo(config);
            }

            VideoFrame frame;
            frame.codec = toBridgeCodec(packet.codec);
            frame.is_keyframe = packet.keyframe;
            frame.width_px = static_cast<uint16_t>(receiver_config.width);
            frame.height_px = static_cast<uint16_t>(receiver_config.height);
            frame.data = packet.data.data();
            frame.len = packet.data.size();
            bridge.publishVideo(frame);
        });

        // Publish decoded PCM audio onto zenoh for the widget's QAudioSink.
        receiver->setAudioHandler([&bridge](const airplay::AudioPacket& packet) {
            AudioChunk chunk;
            chunk.sample_rate_hz = packet.sample_rate;
            chunk.channels = packet.channels;
            // Map the CarPlay audioType category onto the dashboard's stream
            // classes. Everything unrecognised is treated as a nav/alert prompt.
            if (packet.audio_type == "media")
            {
                chunk.stream = AudioStream::Music;
            }
            else if (packet.audio_type == "telephony")
            {
                chunk.stream = AudioStream::Call;
            }
            else if (packet.audio_type == "speechRecognition")
            {
                chunk.stream = AudioStream::Siri;
            }
            else
            {
                chunk.stream = AudioStream::NavPrompt;
            }
            chunk.pcm = packet.data.data();
            chunk.len = packet.data.size();
            bridge.publishAudio(chunk);
        });

        if (!receiver->start())
        {
            SPDLOG_ERROR("[airplay] receiver did not start");
            receiver.reset();
        }
        else
        {
            // Session state combines the recording status and the mic status,
            // both of which change independently; keep the current values in a
            // shared struct so either handler can publish the whole picture.
            const auto config = receiver_config;
            std::atomic<bool>* recording_flag = ctx.recording;
            struct SessionShare
            {
                std::mutex mutex;
                bool recording = false;
                bool mic_active = false;
                uint32_t mic_rate = 0;
                uint8_t mic_channels = 0;
            };
            auto share = std::make_shared<SessionShare>();
            const bool night_mode = options.night_mode;
            const auto publish_session = [&bridge, config, share, night_mode]() {
                std::lock_guard<std::mutex> lock(share->mutex);
                SessionState state;
                state.device_connected = share->recording;
                state.phase = share->recording ? SessionPhase::Recording : SessionPhase::Idle;
                // What the phone was told, so a widget can match its own chrome
                // to the theme CarPlay is drawing inside the video.
                state.night_mode = night_mode;
                state.main_width_px = static_cast<uint16_t>(config.width);
                state.main_height_px = static_cast<uint16_t>(config.height);
                state.mic_active = share->mic_active;
                state.mic_sample_rate_hz = share->mic_rate;
                state.mic_channels = share->mic_channels;
                bridge.publishSession(state);
            };

            receiver->setStatusHandler([recording_flag, share, publish_session](bool recording) {
                if (recording_flag != nullptr)
                {
                    recording_flag->store(recording);
                }
                {
                    std::lock_guard<std::mutex> lock(share->mutex);
                    share->recording = recording;
                }
                publish_session();
            });

            // Mic uplink: when the phone opens a mic stream, tell the widget to
            // start capturing (via session mic_active); the captured PCM comes
            // back on the mic topic and is fed to the receiver's uplink below.
            receiver->setMicStatusHandler(
                [share, publish_session](bool active, uint32_t rate, uint8_t channels) {
                    {
                        std::lock_guard<std::mutex> lock(share->mutex);
                        share->mic_active = active;
                        share->mic_rate = rate;
                        share->mic_channels = channels;
                    }
                    SPDLOG_INFO("[node] microphone {} ({} Hz, {} ch)",
                                active ? "requested" : "released", rate, channels);
                    publish_session();
                });

            // Keyframes are only worth asking the phone for while something is
            // decoding them. This is edge-triggered on nothing-subscribed <->
            // something-subscribed, which covers the case that matters (the
            // dashboard starting after the driver); the receiver's own periodic
            // path still covers a second renderer joining alongside a first.
            airplay::Receiver* keyframe_rx = receiver.get();
            bridge.setVideoSubscriberHandler(
                [keyframe_rx](bool present) { keyframe_rx->setRenderersPresent(present); });
            // The listener only reports *changes*, and the dashboard may well
            // already be up, so seed from the current state rather than a guess.
            receiver->setRenderersPresent(bridge.videoSubscribersPresent());

            // The manufacturer button. Nothing is hooked up to it yet: this is
            // where a "show the vehicle's own UI" action belongs, which for this
            // dashboard means telling the widget stack to leave the CarPlay page.
            receiver->setOemButtonHandler([]() {
                SPDLOG_INFO("[node] manufacturer button pressed -- returning to the vehicle's "
                            "UI is not wired up yet");
            });

            airplay::Receiver* rx = receiver.get();

            // Captured mic PCM from the dashboard -> the phone.
            bridge.setMicHandler([rx](const AudioChunk& chunk) {
                if (chunk.pcm != nullptr && chunk.len > 0)
                {
                    rx->feedMic(std::vector<uint8_t>(chunk.pcm, chunk.pcm + chunk.len));
                }
            });

            // Route the dashboard's touch events to the phone over the event
            // channel. The widget reports x/y in 0..10000 across its area; the
            // receiver wants 0..1.
            bridge.setInputHandler([rx](const InputEvent& event) {
                switch (event.kind)
                {
                    case InputEvent::Kind::TouchDown:
                        rx->sendTouch(event.x / 10000.0f, event.y / 10000.0f,
                                      airplay::Receiver::TouchPhase::Down);
                        break;
                    case InputEvent::Kind::TouchMove:
                        rx->sendTouch(event.x / 10000.0f, event.y / 10000.0f,
                                      airplay::Receiver::TouchPhase::Move);
                        break;
                    case InputEvent::Kind::TouchUp:
                        rx->sendTouch(event.x / 10000.0f, event.y / 10000.0f,
                                      airplay::Receiver::TouchPhase::Up);
                        break;
                    // The rest ride their own HID devices; see airplay/hid.h.
                    // Listed rather than folded into a default so that adding a
                    // new input kind is a compile error here, not a silent drop.
                    case InputEvent::Kind::Knob:
                        sendKnobEvent(*rx, event);
                        break;
                    case InputEvent::Kind::MediaKey:
                        if (!airplay::hid::isKnownMediaKey(event.code))
                        {
                            SPDLOG_WARN("[node] ignoring unknown media key {}", event.code);
                            break;
                        }
                        rx->sendMediaKey(static_cast<airplay::hid::MediaKey>(event.code));
                        break;
                    case InputEvent::Kind::Telephony:
                        if (!airplay::hid::isKnownTelephonyKey(event.code))
                        {
                            SPDLOG_WARN("[node] ignoring unknown telephony key {}", event.code);
                            break;
                        }
                        rx->sendTelephonyKey(static_cast<airplay::hid::TelephonyKey>(event.code));
                        break;
                    case InputEvent::Kind::Siri:
                        rx->requestSiri();
                        break;
                }
            });
        }
    return receiver;
}

// --- Stage 5: iAP2 link layer, identification, MFi auth --------------------
//
// Runs last even though it is stage 5: the phone dials the AirPlay port within
// milliseconds of the CarPlayStartSession this sends, so stages 6 and 7 have to
// be listening before it starts. Blocks until the session ends.
//
// Also owns the metadata path -- now-playing, navigation, calls, and the GPS
// uplink -- because all of it rides the iAP2 carkit channel rather than
// AirPlay. Returns false if the session did not complete.
bool runIap2Stage(const SessionContext& ctx, apple_usb::CarkitChannel& carkit, const AvLink& ncm,
                  ZenohBridge& bridge, std::atomic<bool>& session_stop)
{
    const NodeConfig& options = ctx.options;
    bool ok = true;
        Iap2SessionOptions iap2_options;
        iap2_options.allow_missing_mfi = options.allow_missing_mfi;
        iap2_options.signer = ctx.mfi_signer;
        // The same identity the AirPlay side advertises, by the other route the
        // phone learns it: iAP2 identification rather than GET /info.
        iap2_options.identity = options.vehicle;
        iap2_options.vehicle_status = options.vehicle.status;

        if (ncm.running())
        {
            const AvLink* link = &ncm;
            iap2_options.endpoint_provider =
                [link]() -> std::optional<Iap2SessionOptions::Endpoint> {
                if (!link->running() || link->linkLocalAddress().empty())
                {
                    return std::nullopt;
                }
                Iap2SessionOptions::Endpoint endpoint;
                endpoint.link_local_address = link->linkLocalAddress();
                endpoint.device_identifier = link->hostMac();
                return endpoint;
            };
        }

        // Accumulate now-playing metadata: the phone sends partial updates (a
        // track change carries title/artist/album; a tick may carry only
        // elapsed), so merge each into a persistent state before publishing.
        auto now_playing = std::make_shared<NowPlaying>();
        auto now_playing_mutex = std::make_shared<std::mutex>();
        auto now_playing_valid = std::make_shared<std::atomic<bool>>(false);
        auto now_playing_last_log = std::make_shared<std::string>();
        iap2_options.now_playing_handler = [&bridge, now_playing, now_playing_mutex,
                                            now_playing_valid,
                                            now_playing_last_log](const iap2::NowPlaying& update) {
            std::lock_guard<std::mutex> lock(*now_playing_mutex);
            NowPlaying& np = *now_playing;
            if (update.title)
            {
                np.title = *update.title;
            }
            if (update.artist)
            {
                np.artist = *update.artist;
            }
            if (update.album)
            {
                np.album = *update.album;
            }
            if (update.app_name)
            {
                np.app = *update.app_name;
            }
            if (update.duration_ms)
            {
                np.duration_sec = static_cast<float>(*update.duration_ms) / 1000.0f;
            }
            if (update.elapsed_ms)
            {
                np.elapsed_sec = static_cast<float>(*update.elapsed_ms) / 1000.0f;
            }
            if (update.status)
            {
                np.playing = (*update.status == iap2::PlaybackStatus::kPlaying);
            }
            now_playing_valid->store(true);

            // Announce the track at INFO only when what a user would see
            // actually changes; the phone re-sends the same state ~2/s.
            const std::string signature =
                np.title + '\x1f' + np.artist + '\x1f' + (np.playing ? "1" : "0");
            if (signature != *now_playing_last_log)
            {
                *now_playing_last_log = signature;
                SPDLOG_INFO("[node] now playing: '{}' / '{}' ({})", np.title, np.artist,
                            np.playing ? "playing" : "paused");
            }
            bridge.publishNowPlaying(np);
        };

        // Album artwork arrives asynchronously as a file transfer after a track
        // change. Fold it into the accumulated state and bump the sequence so
        // the widget knows to refresh the image.
        iap2_options.artwork_handler = [&bridge, now_playing, now_playing_mutex,
                                        now_playing_valid](const std::vector<uint8_t>& image) {
            std::lock_guard<std::mutex> lock(*now_playing_mutex);
            NowPlaying& np = *now_playing;
            np.album_art = image;
            ++np.album_art_seq;
            now_playing_valid->store(true);
            bridge.publishNowPlaying(np);
        };

        // Navigation: guidance and maneuver updates are already merged into one
        // iap2::NavGuidance by the session; map it onto the bridge's struct.
        auto nav_state = std::make_shared<NavGuidance>();
        auto nav_mutex = std::make_shared<std::mutex>();
        auto nav_valid = std::make_shared<std::atomic<bool>>(false);
        iap2_options.nav_handler = [&bridge, nav_state, nav_mutex,
                                    nav_valid](const iap2::NavGuidance& g) {
            std::lock_guard<std::mutex> lock(*nav_mutex);
            NavGuidance& nav = *nav_state;
            // A non-zero route-guidance state means guidance is active.
            nav.active = g.status.value_or(0) != 0;
            if (g.road_name)
            {
                nav.road_name = *g.road_name;
            }
            if (g.after_road_name)
            {
                nav.after_road_name = *g.after_road_name;
            }
            if (g.destination_name)
            {
                nav.destination_name = *g.destination_name;
            }
            if (g.maneuver_type)
            {
                nav.maneuver_type = *g.maneuver_type;
            }
            if (g.turn_angle)
            {
                nav.maneuver_angle_deg = *g.turn_angle;
            }
            if (g.junction_type)
            {
                nav.junction_type = *g.junction_type;
            }
            // iap2::NavGuidance names these confusingly:
            //   distance_to_destination = total distance remaining
            //   remain_distance         = distance to the next maneuver
            if (g.distance_to_destination)
            {
                nav.distance_remaining_m = static_cast<float>(*g.distance_to_destination);
            }
            if (g.remain_distance)
            {
                nav.distance_to_maneuver_m = static_cast<float>(*g.remain_distance);
            }
            if (g.time_to_destination)
            {
                nav.time_remaining_sec = static_cast<float>(*g.time_to_destination);
            }
            if (g.eta_epoch)
            {
                nav.eta_epoch_sec = *g.eta_epoch;
            }
            nav_valid->store(true);
            SPDLOG_DEBUG("[node] nav publish: active={} road='{}' dest='{}' toManeuver={}m "
                         "remain={}m eta_in={}s",
                         nav.active, nav.road_name, nav.destination_name,
                         nav.distance_to_maneuver_m, nav.distance_remaining_m,
                         nav.time_remaining_sec);
            bridge.publishNav(nav);
        };

        // Call state: the session's CallTracker already folds per-call updates
        // into a single phase; map that phase onto the bridge's enum.
        auto call_valid = std::make_shared<std::atomic<bool>>(false);
        auto last_call = std::make_shared<CallState>();
        auto call_mutex = std::make_shared<std::mutex>();
        iap2_options.call_handler = [&bridge, call_valid, last_call,
                                     call_mutex](const iap2::CallTracker& tracker) {
            std::lock_guard<std::mutex> lock(*call_mutex);
            CallState& call = *last_call;
            switch (tracker.phase())
            {
                case iap2::CallTracker::Phase::kActive: call.phase = CallPhase::Active; break;
                case iap2::CallTracker::Phase::kRinging: call.phase = CallPhase::Incoming; break;
                case iap2::CallTracker::Phase::kEnded: call.phase = CallPhase::Idle; break;
            }
            call.remote_name = tracker.name();
            call.remote_number = tracker.number();
            call_valid->store(true);
            bridge.publishCall(call);
        };

        // GPS location the phone can dead-reckon from. A GPS source publishes
        // fixes on nodes/carplay/location; cache the latest and hand it to the
        // session, which uplinks NMEA while the phone is asking for location. A
        // --location fix, when given, wins over anything published.
        auto latest_fix = std::make_shared<LocationFix>();
        auto fix_mutex = std::make_shared<std::mutex>();
        auto fix_valid = std::make_shared<std::atomic<bool>>(false);
        if (options.static_location)
        {
            std::lock_guard<std::mutex> lock(*fix_mutex);
            *latest_fix = *options.static_location;
            fix_valid->store(true);
        }
        else
        {
            bridge.setLocationHandler([latest_fix, fix_mutex, fix_valid](const LocationFix& fix) {
                std::lock_guard<std::mutex> lock(*fix_mutex);
                *latest_fix = fix;
                fix_valid->store(true);
            });
        }
        iap2_options.location_provider =
            [latest_fix, fix_mutex, fix_valid]() -> std::optional<iap2::LocationFix> {
            if (!fix_valid->load())
            {
                return std::nullopt;  // no GPS source has published yet
            }
            std::lock_guard<std::mutex> lock(*fix_mutex);
            iap2::LocationFix out;
            out.latitude_deg = latest_fix->latitude_deg;
            out.longitude_deg = latest_fix->longitude_deg;
            out.altitude_m = latest_fix->altitude_m;
            out.speed_knots = latest_fix->speed_knots;
            out.course_deg = latest_fix->course_deg;
            out.satellites = latest_fix->satellites;
            out.hdop = latest_fix->hdop;
            out.utc_epoch_ms = latest_fix->utc_epoch_ms;
            out.valid = latest_fix->valid;
            return out;
        };

        // zenoh has no retained messages, so re-publish the last-known metadata
        // periodically -- otherwise a dashboard that connects between updates
        // (a paused track, a steady navigation screen) shows nothing.
        std::thread metadata_republish([&bridge, now_playing, now_playing_mutex, now_playing_valid,
                                        nav_state, nav_mutex, nav_valid, last_call, call_mutex,
                                        call_valid, &session_stop]() {
            while (!session_stop.load())
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (now_playing_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*now_playing_mutex);
                    bridge.publishNowPlaying(*now_playing);
                }
                if (nav_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*nav_mutex);
                    bridge.publishNav(*nav_state);
                }
                if (call_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*call_mutex);
                    bridge.publishCall(*last_call);
                }
            }
        });

        if (!runIap2Session(carkit, iap2_options, session_stop))
        {
            SPDLOG_ERROR("[iap2] session did not complete");
            ok = false;
        }

        session_stop.store(true);  // release the republish thread if the session ended
        metadata_republish.join();
    return ok;
}

bool runAttachedSession(const apple_usb::DeviceInfo& device, const SessionContext& ctx,
                        ZenohBridge& bridge, std::atomic<bool>& stop)
{
    const NodeConfig& options = ctx.options;
    const std::string& state_dir = ctx.state_dir;
    std::error_code ec;

    // --- Stage 3: usbmux over the vendor-specific interface, then the socket --
    SessionMux mux;
    if (!mux.open(device, state_dir))
    {
        return false;
    }

    // Session-scoped stop: set when the node is shutting down *or* the phone
    // has gone. Everything below waits on this rather than the node-wide flag,
    // so an unplug unwinds one session without ending the process.
    std::atomic<bool> session_stop{false};
    std::thread device_watch([&session_stop, &stop, &mux] {
        while (!session_stop.load())
        {
            if (stop.load() || !mux.alive())
            {
                session_stop.store(true);
                break;
            }
            std::this_thread::sleep_for(kDevicePoll);
        }
    });
    // Any exit from here on has to release the watchdog before returning.
    const auto finish = [&session_stop, &device_watch](bool result) {
        session_stop.store(true);
        if (device_watch.joinable())
        {
            device_watch.join();
        }
        return result;
    };

    const std::string& socket_path = mux.socketPath();

    bool ok = true;
    std::unique_ptr<apple_usb::CarkitChannel> carkit;

    // --- Stage 4: lockdown pairing + the carkit TLS channel -------------------
    if (options.max_stage >= 4)
    {
        // The trust prompt has no deadline; abort only if the node is stopping
        // or the phone was pulled out while we waited.
        carkit = apple_usb::openCarkitChannel(device.serial, socket_path, state_dir,
                                              [&session_stop] { return session_stop.load(); });
        if (!carkit)
        {
            if (!session_stop.load())
            {
                SPDLOG_ERROR("[carkit] could not open the carkit channel for udid={}. "
                             "Delete {} to force a fresh pairing.",
                             shortUdid(device.serial), state_dir);
            }
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[carkit] carkit TLS channel up (iAP2) udid={}",
                        shortUdid(device.serial));
        }
    }
    else
    {
        SPDLOG_INFO("[node] stopping after stage 3 as requested");
    }

    // --- Stage 6: the NCM link ------------------------------------------------
    //
    // Brought up *before* the iAP2 session, not after: the phone asks for the
    // accessory endpoint moments after authentication, and the address only
    // exists once this link is up.
    AvLink ncm;
    if (ok && options.max_stage >= 6)
    {
        if (!ncm.start(device))
        {
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[ncm] {} up, accessory link-local {}", ncm.interfaceName(),
                        ncm.linkLocalAddress());
        }
    }

    // --- Stage 7: the AirPlay RTSP receiver ----------------------------------
    //
    // Started before the iAP2 session for the same reason the NCM bridge is:
    // the phone dials port 7000 within milliseconds of CarPlayStartSession, and
    // anything not listening by then just gets connection-refused.
    // One coprocessor, two consumers: iAP2 authentication and AirPlay
    // /auth-setup. It sits on a single I2C bus, and the two run on different
    // threads, so access is serialised. It is owned by the caller and shared by
    // every session, since it has nothing to do with the phone.
    auto mfi_mutex = ctx.mfi_mutex;

    // Stage 7. Started before stage 5 below: see startAirPlayReceiver.
    std::unique_ptr<airplay::Receiver> receiver;
    if (ok && options.max_stage >= 7)
    {
        receiver = startAirPlayReceiver(ctx, ncm, bridge);
        if (!receiver)
        {
            ok = false;
        }
    }

    // Stage 5. Last, though it is numbered first of the two: see runIap2Stage.
    if (ok && carkit && options.max_stage >= 5)
    {
        ok = runIap2Stage(ctx, *carkit, ncm, bridge, session_stop);
    }

    // Hold the session open so the sockets above can be poked at from another
    // terminal. A failed bring-up falls straight through instead: the caller
    // wants to back off and retry, not sit on a half-open session.
    while (ok && !session_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("[node] tearing down the USB pipeline");
    // Detach the bridge callbacks that capture the receiver before it is
    // destroyed, so a late zenoh mic/input delivery cannot call into a corpse.
    bridge.setMicHandler(nullptr);
    bridge.setInputHandler(nullptr);
    bridge.setLocationHandler(nullptr);
    bridge.setVideoSubscriberHandler(nullptr);
    if (receiver)
    {
        receiver->stop();
    }
    ncm.stop();
    if (carkit)
    {
        carkit->close();
    }
    // Tears down our own mux and socket on Linux; a no-op on macOS, where the
    // socket belongs to the system daemon and must outlive us.
    mux.close();
    return finish(ok);
}

}  // namespace

bool runUsbPipeline(const NodeConfig& options, ZenohBridge& bridge, std::atomic<bool>& stop,
                    std::atomic<bool>* recording)
{
    // Said up front, not when a phone finally turns up: without this the
    // operator plugs in, waits through the detection poll, and only then learns
    // they needed sudo. Not fatal -- stage 1 still enumerates, which is worth
    // having on its own -- so this warns rather than returning.
    if (std::string why_not; !apple_usb::canDetachDevices(why_not))
    {
        SPDLOG_WARN("[usb] {}", why_not);
        SPDLOG_WARN("[usb] a phone already in configuration {} still works from here; only "
                    "switching one into it needs the privilege.",
                    apple_usb::kCarPlayConfiguration);
    }

    const std::string state_dir =
        options.state_dir.empty() ? defaultStateDir() : options.state_dir;

    std::error_code ec;
    fs::create_directories(state_dir, ec);
    if (ec)
    {
        SPDLOG_ERROR("[node] cannot create state dir {}: {}", state_dir, ec.message());
        return false;
    }
    SPDLOG_INFO("[node] state dir {}", state_dir);

    // The coprocessor is on I2C, not USB, so it is initialised once and outlives
    // every phone that comes and goes below.
    auto mfi_signer = std::make_unique<iap2::Mcp2221aMfiSigner>();
    if (!mfi_signer->init())
    {
        SPDLOG_WARN("[mfi] coprocessor unavailable");
        mfi_signer.reset();
    }

    SessionContext ctx{options, state_dir, mfi_signer.get(), std::make_shared<std::mutex>(),
                       recording};

    // Supervisor loop: one pass per phone attachment. A phone can be unplugged
    // and plugged back in as many times as the user likes -- each replug starts
    // a fresh mux, socket, and iAP2 session, because none of that state
    // survives the re-enumeration.
    bool ever_ok = false;
    auto retry_delay = kReattachDelay;
    while (!stop.load())
    {
        const auto found = waitForDevice(stop);
        if (!found)
        {
            break;  // stop was set while waiting
        }

        if (options.max_stage < 3)
        {
            SPDLOG_INFO("[node] stopping after stage 2 as requested");
            return switchToCarPlay(*found).has_value();
        }

        bool attached = false;
        if (const auto device = switchToCarPlay(*found))
        {
            attached = runAttachedSession(*device, ctx, bridge, stop);
            ever_ok |= attached;
        }
        if (stop.load())
        {
            break;
        }

        // Tell the dashboard the phone is gone, rather than leaving the last
        // frame and track up. The recording flag has to be cleared too or the
        // idle publisher in main() keeps deferring to a session that has ended.
        if (ctx.recording != nullptr)
        {
            ctx.recording->store(false);
        }
        bridge.publishSession(SessionState{});

        // Back off when the same phone keeps failing -- a phone whose owner
        // tapped "Don't Trust" would otherwise re-run the whole bring-up every
        // two seconds forever. A clean attach, or the phone being unplugged,
        // resets the delay so a genuine replug is picked up immediately.
        if (attached || apple_usb::listAppleDevices().empty())
        {
            retry_delay = kReattachDelay;
        }
        else
        {
            retry_delay = std::min(retry_delay * 2, kMaxReattachDelay);
            SPDLOG_WARN("[node] bring-up failed with the phone still attached; retrying in {}s",
                        retry_delay.count());
        }

        SPDLOG_INFO("[node] session ended; waiting for a phone to be plugged in");
        for (auto waited = std::chrono::seconds(0); waited < retry_delay && !stop.load();
             waited += std::chrono::seconds(1))
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    SPDLOG_INFO("[node] USB pipeline stopped");
    return ever_ok;
}

}  // namespace carplay
