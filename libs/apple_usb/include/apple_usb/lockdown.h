// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/carkit.py
#ifndef APPLE_USB_LOCKDOWN_H_
#define APPLE_USB_LOCKDOWN_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apple_usb
{

// The TLS iAP2 control channel obtained from com.apple.carkit.service. Reads
// and writes raw iAP2 link-layer bytes; the iap2 library sits on top of this.
class CarkitChannel
{
  public:
    virtual ~CarkitChannel() = default;

    // Blocking write of the whole buffer. Returns false on error/closed.
    virtual bool send(const uint8_t* data, size_t len) = 0;

    // Blocking read of up to max_len bytes. Returns empty on EOF/error.
    virtual std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) = 0;

    virtual void close() = 0;

    // False once the underlying connection has failed. recv() returns an empty
    // vector both for "timed out with no data" and for a dead connection, so a
    // caller that treats empty as "no data yet" -- the iAP2 link layer does --
    // needs this to tell the two apart. Without it a dead link spins forever.
    virtual bool alive() const = 0;
};

// Establish the carkit iAP2 channel for a phone reachable through the given
// usbmuxd-compatible socket (our UsbmuxdServer). Runs lockdown pairing +
// client-cert TLS via libimobiledevice. Returns nullptr on failure.
//
// Compiled only when libimobiledevice is available (see CMakeLists);
// otherwise this returns nullptr with a log message.
std::unique_ptr<CarkitChannel> openCarkitChannel(const std::string& udid,
                                                  const std::string& usbmux_socket_path,
                                                  const std::string& pair_record_dir);

}  // namespace apple_usb

#endif  // APPLE_USB_LOCKDOWN_H_
