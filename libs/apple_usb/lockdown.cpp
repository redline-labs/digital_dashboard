// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/carkit.py
//
// Uses libimobiledevice for lockdown pairing + client-cert TLS + starting
// com.apple.carkit.service, pointed at our own config-6 usbmux socket via
// USBMUXD_SOCKET_ADDRESS. This is the same delegation LIVI makes to
// pymobiledevice3 -- we do not reimplement lockdown ourselves.
#include "apple_usb/lockdown.h"

#include <spdlog/spdlog.h>

#ifdef APPLE_USB_HAVE_LIBIMOBILEDEVICE

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

namespace apple_usb
{

namespace
{

const char* kCarkitService = "com.apple.carkit.service";

// How long to keep retrying errors that should clear up on their own (the mux
// settling after the configuration switch, a pair record the phone rejected).
// Waiting on the *user* is not bounded by this -- see waitingOnUser().
constexpr auto kTransientTimeout = std::chrono::seconds(60);
constexpr auto kHandshakeRetryDelay = std::chrono::milliseconds(1000);

// A first-time pair puts "Trust This Computer?" on the phone and lockdownd
// answers PAIRING_DIALOG_RESPONSE_PENDING until the user taps it. There is no
// sensible deadline for that -- the phone may well be in a pocket -- so we wait
// for as long as the node is running and just re-state what we are waiting for
// now and then.
constexpr auto kUserReminderInterval = std::chrono::seconds(30);

class LibimobiledeviceCarkitChannel : public CarkitChannel
{
  public:
    LibimobiledeviceCarkitChannel(idevice_t device, lockdownd_client_t lockdown,
                                  idevice_connection_t connection) :
        device_(device), lockdown_(lockdown), connection_(connection)
    {
    }

    ~LibimobiledeviceCarkitChannel() override { close(); }

    bool send(const uint8_t* data, size_t len) override
    {
        size_t sent_total = 0;
        while (sent_total < len)
        {
            uint32_t sent = 0;
            const idevice_error_t err = idevice_connection_send(
                connection_, reinterpret_cast<const char*>(data) + sent_total,
                static_cast<uint32_t>(len - sent_total), &sent);
            if (err != IDEVICE_E_SUCCESS || sent == 0)
            {
                SPDLOG_DEBUG("[carkit] send failed after {}/{} bytes (err {})", sent_total, len,
                             static_cast<int>(err));
                alive_ = false;
                return false;
            }
            sent_total += sent;
        }
        return true;
    }

    std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) override
    {
        std::vector<uint8_t> buf(max_len);
        uint32_t received = 0;
        const idevice_error_t err = idevice_connection_receive_timeout(
            connection_, reinterpret_cast<char*>(buf.data()), static_cast<uint32_t>(max_len),
            &received, timeout_ms);
        if (err != IDEVICE_E_SUCCESS && err != IDEVICE_E_TIMEOUT)
        {
            // Distinguished from a timeout only through alive(); the empty
            // return value looks identical to the caller.
            SPDLOG_DEBUG("[carkit] receive failed (err {})", static_cast<int>(err));
            alive_ = false;
            return {};
        }
        buf.resize(received);
        return buf;
    }

    bool alive() const override { return alive_; }

    void close() override
    {
        alive_ = false;
        if (connection_ != nullptr)
        {
            idevice_disconnect(connection_);
            connection_ = nullptr;
        }
        if (lockdown_ != nullptr)
        {
            lockdownd_client_free(lockdown_);
            lockdown_ = nullptr;
        }
        if (device_ != nullptr)
        {
            idevice_free(device_);
            device_ = nullptr;
        }
    }

  private:
    idevice_t device_;
    lockdownd_client_t lockdown_;
    idevice_connection_t connection_;
    bool alive_ = true;
};

// libusbmuxd normalises a modern 24-character serial into the 25-character
// "XXXXXXXX-XXXXXXXXXXXXXXXX" form, and idevice_new_with_options() matches the
// requested UDID against that normalised string. The serial we read from sysfs
// carries no dash, so passing it through unchanged makes the lookup fail with a
// misleading "device not found" even though the device is right there.
// Verified on hardware: the undashed form reports "Device ... not found!" while
// the dashed form reaches lockdownd.
std::string normalizeUdid(const std::string& udid)
{
    if (udid.size() == 24 && udid.find('-') == std::string::npos)
    {
        return udid.substr(0, 8) + "-" + udid.substr(8);
    }
    return udid;
}

// Whether waiting and trying the handshake again can plausibly succeed. The
// pending/locked cases are the normal first-pair path; the pair-record ones
// resolve themselves because libimobiledevice re-pairs when the record it holds
// is rejected. Everything else (the user tapping "Don't Trust", a supervised
// device that refuses to pair over USB) will fail identically forever.
bool retryableHandshakeError(lockdownd_error_t err)
{
    static constexpr lockdownd_error_t kRetryable[] = {
        LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING,  // dialog is up, no answer yet
        LOCKDOWN_E_PASSWORD_PROTECTED,               // phone is locked
        LOCKDOWN_E_INVALID_HOST_ID,                  // stale record -> re-pair
        LOCKDOWN_E_MISSING_HOST_ID,
        LOCKDOWN_E_INVALID_PAIR_RECORD,
        LOCKDOWN_E_MISSING_PAIR_RECORD,
        LOCKDOWN_E_INVALID_CONF,
        LOCKDOWN_E_SSL_ERROR,
        LOCKDOWN_E_MUX_ERROR,  // the mux is still settling after the config switch
        LOCKDOWN_E_RECEIVE_TIMEOUT,
    };
    for (const lockdownd_error_t candidate : kRetryable)
    {
        if (err == candidate)
        {
            return true;
        }
    }
    return false;
}

// Errors that mean the phone is waiting on its owner rather than on us. These
// are retried for as long as the node runs, not against a deadline.
bool waitingOnUser(lockdownd_error_t err)
{
    return err == LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING ||
           err == LOCKDOWN_E_PASSWORD_PROTECTED;
}

// Runs the lockdown handshake, waiting out the Trust dialog. Returns the client
// on success, or nullptr when `abort` fires, the transient budget runs out, or
// the error is terminal.
lockdownd_client_t handshakeWithRetry(idevice_t device, const std::string& udid,
                                      const std::function<bool()>& abort)
{
    auto transient_deadline = std::chrono::steady_clock::now() + kTransientTimeout;
    auto next_reminder = std::chrono::steady_clock::time_point::min();
    lockdownd_error_t err = LOCKDOWN_E_UNKNOWN_ERROR;
    bool waited_on_user = false;

    for (;;)
    {
        if (abort && abort())
        {
            SPDLOG_INFO("[carkit] stopped waiting for the lockdown handshake on udid={}",
                        udid.substr(0, 8));
            return nullptr;
        }

        lockdownd_client_t lockdown = nullptr;
        err = lockdownd_client_new_with_handshake(device, &lockdown, "mercedes-carplay");
        if (err == LOCKDOWN_E_SUCCESS && lockdown != nullptr)
        {
            if (waited_on_user)
            {
                SPDLOG_INFO("[carkit] the phone trusts this computer now");
            }
            return lockdown;
        }
        if (lockdown != nullptr)
        {
            lockdownd_client_free(lockdown);
        }

        if (!retryableHandshakeError(err))
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (waitingOnUser(err))
        {
            waited_on_user = true;
            // No deadline: keep asking until the phone is trusted or the node is
            // torn down. Repeat the prompt occasionally rather than every second,
            // which would bury everything else in the log.
            if (now >= next_reminder)
            {
                next_reminder = now + kUserReminderInterval;
                if (err == LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING)
                {
                    SPDLOG_WARN("[carkit] the phone is asking to trust this computer -- tap "
                                "Trust on it; still waiting");
                }
                else
                {
                    SPDLOG_WARN("[carkit] the phone is locked; unlock it so it can show the "
                                "trust prompt; still waiting");
                }
            }
            // A slow tap must not eat the budget reserved for transient faults.
            transient_deadline = now + kTransientTimeout;
        }
        else
        {
            if (now >= transient_deadline)
            {
                SPDLOG_ERROR("[carkit] gave up after {}s waiting for the lockdown handshake: "
                             "{} ({})", kTransientTimeout.count(), lockdownd_strerror(err),
                             static_cast<int>(err));
                return nullptr;
            }
            if (now >= next_reminder)
            {
                next_reminder = now + kUserReminderInterval;
                SPDLOG_WARN("[carkit] lockdown handshake not ready yet: {} ({}); retrying",
                            lockdownd_strerror(err), static_cast<int>(err));
            }
        }

        std::this_thread::sleep_for(kHandshakeRetryDelay);
    }

    SPDLOG_ERROR("[carkit] lockdown handshake failed for udid={}: {} ({})", udid.substr(0, 8),
                 lockdownd_strerror(err), static_cast<int>(err));
    if (err == LOCKDOWN_E_USER_DENIED_PAIRING)
    {
        SPDLOG_ERROR("[carkit] the trust prompt was declined on the phone. Unplug it, forget "
                     "this computer under Settings > General > Transfer or Reset, and retry.");
    }
    return nullptr;
}

}  // namespace

std::unique_ptr<CarkitChannel> openCarkitChannelViaLibimobiledevice(
    const std::string& udid, const std::string& usbmux_socket_path,
    const std::string& pair_record_dir, const std::function<bool()>& abort)
{
    // Point libusbmuxd at our config-6 mux socket instead of the system usbmuxd.
    // The pair records follow automatically: libimobiledevice asks whichever
    // usbmuxd it is talking to for them (ReadPairRecord/SavePairRecord), and our
    // UsbmuxdServer answers those out of pair_record_dir. libimobiledevice 1.4.0
    // has no environment override for its own on-disk fallback store, so that
    // path is only reached if our server stops answering.
    (void)pair_record_dir;
    const std::string mux_addr = "UNIX:" + usbmux_socket_path;
    ::setenv("USBMUXD_SOCKET_ADDRESS", mux_addr.c_str(), 1);

    const std::string lookup_udid = normalizeUdid(udid);

    idevice_t device = nullptr;
    if (idevice_new_with_options(&device, lookup_udid.c_str(), IDEVICE_LOOKUP_USBMUX) !=
            IDEVICE_E_SUCCESS ||
        device == nullptr)
    {
        SPDLOG_ERROR("[carkit] idevice_new failed for udid={}", lookup_udid.substr(0, 8));
        return nullptr;
    }

    lockdownd_client_t lockdown = handshakeWithRetry(device, udid, abort);
    if (lockdown == nullptr)
    {
        idevice_free(device);
        return nullptr;
    }

    lockdownd_service_descriptor_t service = nullptr;
    const lockdownd_error_t service_err = lockdownd_start_service(lockdown, kCarkitService, &service);
    if (service_err != LOCKDOWN_E_SUCCESS || service == nullptr)
    {
        SPDLOG_ERROR("[carkit] could not start {}: {} ({})", kCarkitService,
                     lockdownd_strerror(service_err), static_cast<int>(service_err));
        lockdownd_client_free(lockdown);
        idevice_free(device);
        return nullptr;
    }

    idevice_connection_t connection = nullptr;
    const bool need_ssl = service->ssl_enabled != 0;
    const idevice_error_t conn_err = idevice_connect(device, service->port, &connection);
    lockdownd_service_descriptor_free(service);
    if (conn_err != IDEVICE_E_SUCCESS || connection == nullptr)
    {
        SPDLOG_ERROR("[carkit] connect to carkit port failed");
        lockdownd_client_free(lockdown);
        idevice_free(device);
        return nullptr;
    }

    if (need_ssl && idevice_connection_enable_ssl(connection) != IDEVICE_E_SUCCESS)
    {
        SPDLOG_ERROR("[carkit] enable SSL on carkit channel failed");
        idevice_disconnect(connection);
        lockdownd_client_free(lockdown);
        idevice_free(device);
        return nullptr;
    }

    SPDLOG_INFO("[carkit] carkit TLS channel up (iAP2) udid={}", udid.substr(0, 8));
    return std::make_unique<LibimobiledeviceCarkitChannel>(device, lockdown, connection);
}

}  // namespace apple_usb

#else  // APPLE_USB_HAVE_LIBIMOBILEDEVICE

namespace apple_usb
{

std::unique_ptr<CarkitChannel> openCarkitChannelViaLibimobiledevice(
    const std::string&, const std::string&, const std::string&, const std::function<bool()>&)
{
    SPDLOG_ERROR("[carkit] libimobiledevice not available at build time; only the native "
                 "lockdown backend can be used, and it cannot pair a new device");
    return nullptr;
}

}  // namespace apple_usb

#endif  // APPLE_USB_HAVE_LIBIMOBILEDEVICE
