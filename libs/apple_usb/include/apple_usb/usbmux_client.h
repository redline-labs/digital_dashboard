// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_USBMUX_CLIENT_H_
#define APPLE_USB_USBMUX_CLIENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace apple_usb
{

// The client half of the usbmux plist protocol -- the side libusbmuxd occupies
// today. It talks to whatever unix socket it is pointed at, which in this
// project is our own UsbmuxdServer sitting on the config-6 mux.
//
// Both ends of this protocol now live here, which is the reason to own it: the
// server was already ours, so the client is the smaller half of a conversation
// we fully control, and it removes the USBMUXD_SOCKET_ADDRESS environment
// variable and the UDID dash normalisation that only existed to satisfy
// libusbmuxd's own lookup.

// One device as reported by ListDevices.
struct MuxDevice
{
    uint32_t device_id = 0;
    std::string serial;
    std::string connection_type;
    uint32_t product_id = 0;
};

// A byte stream to a port on the device. After Connect succeeds the control
// socket stops carrying plists and becomes this pipe, so the fd is the
// connection -- there is no way to issue another request on it.
class MuxConnection
{
  public:
    explicit MuxConnection(int fd);
    ~MuxConnection();

    MuxConnection(const MuxConnection&) = delete;
    MuxConnection& operator=(const MuxConnection&) = delete;

    // Writes the whole buffer. False on error or a closed peer.
    bool sendAll(const uint8_t* data, size_t len);

    // Reads up to max_len bytes, waiting at most timeout_ms. Returns the count,
    // 0 on timeout, and -1 on error or EOF -- a caller polling for data has to
    // tell "nothing yet" from "never again".
    ssize_t recvSome(uint8_t* out, size_t max_len, unsigned timeout_ms);

    // The underlying socket, for handing to a TLS layer. Still owned here.
    int fd() const { return fd_; }

    void close();

  private:
    int fd_ = -1;
};

class UsbmuxClient
{
  public:
    explicit UsbmuxClient(std::string socket_path);

    // Each call opens its own short-lived control connection. usbmux has no
    // notion of a persistent session, and Connect consumes the socket it runs
    // on, so there is nothing to keep open between requests.
    std::optional<std::string> readBuid();
    std::vector<MuxDevice> listDevices();
    std::optional<std::vector<uint8_t>> readPairRecord(const std::string& record_id);
    bool savePairRecord(const std::string& record_id, const std::vector<uint8_t>& data);

    // Opens a stream to `port` (host byte order) on the device. Returns nullptr
    // when the mux refuses.
    std::unique_ptr<MuxConnection> connect(uint32_t device_id, uint16_t port);

    // The device whose serial matches, comparing both the dashed and undashed
    // spellings of a 24-character UDID. Callers hold whichever form sysfs gave
    // them, and no longer have to know which one the mux reports.
    std::optional<MuxDevice> findDevice(const std::string& udid);

    const std::string& socketPath() const { return socket_path_; }

  private:
    std::string socket_path_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_USBMUX_CLIENT_H_
