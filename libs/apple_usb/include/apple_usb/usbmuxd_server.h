// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py (UsbmuxdServer)
#ifndef APPLE_USB_USBMUXD_SERVER_H_
#define APPLE_USB_USBMUXD_SERVER_H_

#include "apple_usb/muxd.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// libplist's plist_t is an opaque `void*`; repeating the typedef verbatim keeps
// this header free of the libplist dependency and is a legal redeclaration when
// <plist/plist.h> is also in scope.
typedef void* plist_t;

namespace apple_usb
{

// Exposes a MuxHost through a unix-domain socket that speaks the standard
// usbmuxd plist protocol (ListDevices / Connect / ReadBUID / Read+SavePairRecord).
// libimobiledevice is pointed at this socket via USBMUXD_SOCKET_ADDRESS so it
// can run lockdown + TLS on top of our config-6 mux, exactly as LIVI points
// pymobiledevice3 at its own socket.
//
// Pair records and the system BUID are stored as .plist files under state_dir
// (mirrors /var/lib/lockdown), so a phone re-pairs only once.
class UsbmuxdServer
{
  public:
    UsbmuxdServer(MuxHost& host, std::string socket_path, std::string state_dir);
    ~UsbmuxdServer();

    bool start();
    void stop();

    const std::string& socketPath() const { return socket_path_; }

  private:
    void acceptLoop();
    void clientLoop(int client_fd);
    void relay(int client_fd, std::shared_ptr<MuxTcpConn> conn);

    // Pair-record / BUID store, keyed under state_dir_ (mirrors /var/lib/lockdown).
    std::string readBuid();
    std::vector<uint8_t> readPairRecord(const std::string& udid);
    void savePairRecord(const std::string& udid, const uint8_t* data, size_t len);
    plist_t deviceEntry();

    MuxHost& host_;
    std::string socket_path_;
    std::string state_dir_;
    int server_fd_ = -1;
    std::atomic<bool> run_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_USBMUXD_SERVER_H_
