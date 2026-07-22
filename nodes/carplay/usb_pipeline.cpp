// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_pipeline.h"

#include "iap2_session.h"

#include "apple_usb/lockdown.h"
#include "apple_usb/muxd.h"
#include "apple_usb/ncm_bridge.h"

#include "airplay/receiver.h"
#include "iap2/mcp2221a_mfi_signer.h"
#include "apple_usb/usb_device.h"
#include "apple_usb/usbmuxd_server.h"

#include <spdlog/spdlog.h>

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

std::string shortUdid(const std::string& udid)
{
    return udid.size() > 8 ? udid.substr(0, 8) : udid;
}

// The configuration switch re-enumerates the phone, which invalidates the
// usbfs path captured before it. Everything downstream opens that path, so the
// DeviceInfo has to be re-read afterwards rather than reused.
std::optional<apple_usb::DeviceInfo> rediscover(const std::string& serial)
{
    const auto deadline = std::chrono::steady_clock::now() + kRediscoverTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& device : apple_usb::listAppleDevices())
        {
            if (device.serial == serial)
            {
                return device;
            }
        }
        std::this_thread::sleep_for(kRediscoverPoll);
    }
    return std::nullopt;
}

std::string defaultStateDir()
{
    if (const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR"); runtime_dir != nullptr)
    {
        return (fs::path(runtime_dir) / "carplay").string();
    }
    return (fs::temp_directory_path() / "carplay").string();
}

// --- Stage 2: device detection and the CarPlay configuration switch ---------
std::optional<apple_usb::DeviceInfo> stageDetectAndSwitch()
{
    const auto devices = apple_usb::listAppleDevices();
    if (devices.empty())
    {
        SPDLOG_ERROR("[usb] no Apple device found. Is the phone plugged in, unlocked and "
                     "trusted? On a VM, confirm it is attached to the guest.");
        return std::nullopt;
    }

    for (const auto& device : devices)
    {
        SPDLOG_INFO("[usb] found {:04x}:{:04x} udid={} at {} (config {})",
                    device.vid, device.pid, device.serial, device.usbfs_path,
                    device.active_configuration);
    }

    apple_usb::DeviceInfo device = devices.front();
    if (devices.size() > 1)
    {
        SPDLOG_WARN("[usb] {} Apple devices present; using {}", devices.size(), device.serial);
    }

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

    const auto refreshed = rediscover(device.serial);
    if (!refreshed)
    {
        SPDLOG_ERROR("[usb] udid={} did not come back after the configuration switch. On a "
                     "VM this usually means the hypervisor handed the re-enumerating device "
                     "back to the host.", shortUdid(device.serial));
        return std::nullopt;
    }

    if (refreshed->active_configuration != apple_usb::kCarPlayConfiguration)
    {
        SPDLOG_ERROR("[usb] udid={} is in configuration {}, expected {}. Something "
                     "re-enumerated it -- check that the system usbmuxd is stopped.",
                     shortUdid(refreshed->serial), refreshed->active_configuration,
                     apple_usb::kCarPlayConfiguration);
        return std::nullopt;
    }

    SPDLOG_INFO("[usb] udid={} now in configuration {} at {}",
                shortUdid(refreshed->serial), refreshed->active_configuration,
                refreshed->usbfs_path);
    return refreshed;
}

}  // namespace

bool runUsbPipeline(const UsbPipelineOptions& options, std::atomic<bool>& stop)
{
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

    // --- Stage 2 ---
    const auto device = stageDetectAndSwitch();
    if (!device)
    {
        return false;
    }
    if (options.max_stage < 3)
    {
        SPDLOG_INFO("[node] stopping after stage 2 as requested");
        return true;
    }

    // --- Stage 3: usbmux over the vendor-specific interface, then the socket --
    apple_usb::MuxHost mux(*device);
    if (!mux.open())
    {
        SPDLOG_ERROR("[muxd] could not open the mux for udid={}. Another driver may hold "
                     "the interface -- check `lsusb -t` and that the system usbmuxd is "
                     "stopped.", shortUdid(device->serial));
        return false;
    }
    SPDLOG_INFO("[muxd] mux open for udid={}", shortUdid(device->serial));

    const std::string socket_path =
        (fs::path(state_dir) / ("usbmuxd-" + shortUdid(device->serial) + ".sock")).string();
    fs::remove(socket_path, ec);

    apple_usb::UsbmuxdServer usbmuxd(mux, socket_path, state_dir);
    if (!usbmuxd.start())
    {
        SPDLOG_ERROR("[usbmuxd] could not serve on {}", socket_path);
        mux.close();
        return false;
    }
    SPDLOG_INFO("[usbmuxd] serving {} on {}", shortUdid(device->serial), socket_path);
    SPDLOG_INFO("[usbmuxd] sanity-check it independently with:");
    SPDLOG_INFO("[usbmuxd]   USBMUXD_SOCKET_ADDRESS=UNIX:{} idevice_id -l", socket_path);

    bool ok = true;
    std::unique_ptr<apple_usb::CarkitChannel> carkit;

    // --- Stage 4: lockdown pairing + the carkit TLS channel -------------------
    if (options.max_stage >= 4)
    {
        carkit = apple_usb::openCarkitChannel(device->serial, socket_path, state_dir);
        if (!carkit)
        {
            SPDLOG_ERROR("[carkit] could not open the carkit channel for udid={}. If the "
                         "phone is asking to trust this computer, tap Trust and retry; "
                         "delete {} to force a fresh pairing.",
                         shortUdid(device->serial), state_dir);
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[carkit] carkit TLS channel up (iAP2) udid={}",
                        shortUdid(device->serial));
        }
    }
    else
    {
        SPDLOG_INFO("[node] stopping after stage 3 as requested");
    }

    // --- Stage 6: NCM <-> TAP link -------------------------------------------
    //
    // Brought up *before* the iAP2 session, not after: the phone asks for the
    // accessory endpoint moments after authentication, and the address only
    // exists once this bridge is running.
    std::unique_ptr<apple_usb::NcmBridge> ncm;
    if (ok && options.max_stage >= 6)
    {
        ncm = std::make_unique<apple_usb::NcmBridge>(*device);
        if (!ncm->start())
        {
            SPDLOG_ERROR("[ncm] bridge did not start. Needs CAP_NET_ADMIN for the TAP device "
                         "(setcap cap_net_admin+ep on this binary, or run as root).");
            ncm.reset();
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[ncm] {} up, accessory link-local {}", ncm->interfaceName(),
                        ncm->linkLocalAddress());
        }
    }

    // --- Stage 7: the AirPlay RTSP receiver ----------------------------------
    //
    // Started before the iAP2 session for the same reason the NCM bridge is:
    // the phone dials port 7000 within milliseconds of CarPlayStartSession, and
    // anything not listening by then just gets connection-refused.
    // One coprocessor, two consumers: iAP2 authentication and AirPlay
    // /auth-setup. It sits on a single I2C bus, and the two run on different
    // threads, so access is serialised.
    auto mfi_signer = std::make_unique<iap2::Mcp2221aMfiSigner>();
    auto mfi_mutex = std::make_shared<std::mutex>();
    if (!mfi_signer->init())
    {
        SPDLOG_WARN("[mfi] coprocessor unavailable");
        mfi_signer.reset();
    }

    std::unique_ptr<airplay::Receiver> receiver;
    if (ok && options.max_stage >= 7)
    {
        airplay::ReceiverConfig receiver_config;
        if (mfi_signer)
        {
            iap2::MfiSigner* signer = mfi_signer.get();
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
        if (!receiver->start())
        {
            SPDLOG_ERROR("[airplay] receiver did not start");
            receiver.reset();
            ok = false;
        }
    }

    // --- Stage 5: iAP2 link layer, identification, MFi auth ------------------
    if (ok && carkit && options.max_stage >= 5)
    {
        Iap2SessionOptions iap2_options;
        iap2_options.allow_missing_mfi = options.allow_missing_mfi;
        iap2_options.signer = mfi_signer.get();

        if (ncm)
        {
            apple_usb::NcmBridge* bridge = ncm.get();
            iap2_options.endpoint_provider =
                [bridge]() -> std::optional<Iap2SessionOptions::Endpoint> {
                if (!bridge->running() || bridge->linkLocalAddress().empty())
                {
                    return std::nullopt;
                }
                Iap2SessionOptions::Endpoint endpoint;
                endpoint.link_local_address = bridge->linkLocalAddress();
                endpoint.device_identifier = bridge->hostMac();
                return endpoint;
            };
        }

        if (!runIap2Session(*carkit, iap2_options, stop))
        {
            SPDLOG_ERROR("[iap2] session did not complete");
            ok = false;
        }
    }

    // Hold the session open so the sockets above can be poked at from another
    // terminal; the stages beyond this one are not implemented yet.
    while (!stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("[node] tearing down the USB pipeline");
    if (receiver)
    {
        receiver->stop();
    }
    if (ncm)
    {
        ncm->stop();
    }
    if (carkit)
    {
        carkit->close();
    }
    usbmuxd.stop();
    mux.close();
    fs::remove(socket_path, ec);
    return ok;
}

}  // namespace carplay
