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

#include <cstdlib>

namespace apple_usb
{

namespace
{

const char* kCarkitService = "com.apple.carkit.service";

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

}  // namespace

std::unique_ptr<CarkitChannel> openCarkitChannel(const std::string& udid,
                                                 const std::string& usbmux_socket_path,
                                                 const std::string& pair_record_dir)
{
    // Point libusbmuxd at our config-6 mux socket, and lockdown at our pair
    // records, instead of the system usbmuxd/lockdown stores.
    const std::string mux_addr = "UNIX:" + usbmux_socket_path;
    ::setenv("USBMUXD_SOCKET_ADDRESS", mux_addr.c_str(), 1);
    if (!pair_record_dir.empty())
    {
        ::setenv("USBMUXD_LOCKDOWN_PATH", pair_record_dir.c_str(), 1);
    }

    const std::string lookup_udid = normalizeUdid(udid);

    idevice_t device = nullptr;
    if (idevice_new_with_options(&device, lookup_udid.c_str(), IDEVICE_LOOKUP_USBMUX) !=
            IDEVICE_E_SUCCESS ||
        device == nullptr)
    {
        SPDLOG_ERROR("[carkit] idevice_new failed for udid={}", lookup_udid.substr(0, 8));
        return nullptr;
    }

    lockdownd_client_t lockdown = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown, "mercedes-carplay") != LOCKDOWN_E_SUCCESS ||
        lockdown == nullptr)
    {
        SPDLOG_ERROR("[carkit] lockdown handshake failed for udid={}", udid.substr(0, 8));
        idevice_free(device);
        return nullptr;
    }

    lockdownd_service_descriptor_t service = nullptr;
    if (lockdownd_start_service(lockdown, kCarkitService, &service) != LOCKDOWN_E_SUCCESS ||
        service == nullptr)
    {
        SPDLOG_ERROR("[carkit] could not start {}", kCarkitService);
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

std::unique_ptr<CarkitChannel> openCarkitChannel(const std::string&, const std::string&, const std::string&)
{
    SPDLOG_ERROR("[carkit] libimobiledevice not available at build time; wired CarPlay lockdown is disabled");
    return nullptr;
}

}  // namespace apple_usb

#endif  // APPLE_USB_HAVE_LIBIMOBILEDEVICE
