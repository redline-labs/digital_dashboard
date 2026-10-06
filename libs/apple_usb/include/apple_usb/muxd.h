// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/muxd.py
#ifndef APPLE_USB_MUXD_H_
#define APPLE_USB_MUXD_H_

#include "apple_usb/usb_device.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apple_usb
{

class MuxHost;

// The bulk pipe usbmux runs over: the phone's endpoints in production, a fake in
// the tests. Both calls throw std::system_error; read() throws ETIMEDOUT when
// nothing arrived in time.
class MuxTransport
{
  public:
    virtual ~MuxTransport() = default;
    virtual void write(const uint8_t* data, size_t len) = 0;
    virtual std::vector<uint8_t> read(unsigned timeout_ms) = 0;
};

// A single TCP-over-USB stream to the phone, riding Apple's usbmux protocol.
// Mirrors MuxTcpConn in muxd.py: a minimal TCP state machine (SYN/ACK/FIN/RST)
// where the accessory is the active opener.
class MuxTcpConn
{
  public:
    MuxTcpConn(MuxHost& host, uint16_t sport, uint16_t dport);

    // Blocking send/recv over the stream. recv returns an empty buffer on EOF;
    // send returns false once the transport has failed.
    bool send(const uint8_t* data, size_t len);
    std::vector<uint8_t> recv();
    void close();

    bool closed() const { return closed_.load(); }
    uint16_t sport() const { return sport_; }

    // Abandon the stream without touching USB, and release anyone blocked in
    // recv()/waitConnected(). Used when the transport underneath has gone away
    // (an unplug), where sending a FIN would only throw.
    void fail();

    // Called by the host reader thread when a segment for this stream arrives.
    void onPacket(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t win,
                  const uint8_t* payload, size_t payload_len);

    // Blocks until the SYN handshake completes or fails; returns success.
    bool waitConnected();

  private:
    friend class MuxHost;
    bool sendTcp(uint8_t flags, const uint8_t* payload = nullptr, size_t payload_len = 0);

    MuxHost& host_;
    uint16_t sport_;
    uint16_t dport_;
    uint32_t tx_seq_ = 0;
    uint32_t tx_ack_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> rq_;
    bool connected_ = false;
    std::atomic<bool> closed_{false};
};

// Owns the USB handle for a phone in the CarPlay configuration and runs the
// userspace usbmux multiplexer (magic 0xFEEDFACE) over bulk endpoints.
class MuxHost
{
  public:
    explicit MuxHost(DeviceInfo device);
    // A host over a pipe that is already open, claimed by nobody. What the
    // tests use; open() then skips everything USB.
    explicit MuxHost(std::unique_ptr<MuxTransport> transport);
    ~MuxHost();

    // Locate the usbmux interface from the configuration descriptor, claim it,
    // and start the reader thread. Returns success.
    bool open();
    void close();

    // Open a new TCP-over-USB stream to a device port (host byte order).
    std::shared_ptr<MuxTcpConn> connect(uint16_t dport);

    const std::string& serial() const { return device_.serial; }

    // False once the reader thread has stopped or a write has failed. The
    // usual cause is the phone being unplugged, which surfaces as an I/O error
    // on either endpoint; callers poll this to notice a disconnect and tear the
    // session down.
    bool alive() const
    {
        return run_.load() && !reader_stopped_.load() && !write_failed_.load();
    }

  private:
    friend class MuxTcpConn;

    // Serialized so mux sequence numbers and bulk writes stay consistent.
    // Never throws: false once the transport has failed.
    bool muxSend(uint32_t proto, const uint8_t* payload, size_t payload_len);
    void readerLoop();

    // The version and setup exchange, then the reader. Shared by both kinds
    // of host.
    bool startMux();
    void releaseUsb();

    // Releases every stream still waiting on the transport, as an unplug does.
    void failConnections();

    // Fills iface_/ep_in_/ep_out_ from the active configuration descriptor.
    bool locateMuxInterface();

    DeviceInfo device_;
    DeviceHandle handle_;
    // False for a host built over an injected transport.
    bool usb_ = true;
    std::unique_ptr<MuxTransport> transport_;

    // Discovered in open() rather than hardcoded; see locateMuxInterface().
    uint8_t iface_ = 0;
    uint8_t ep_in_ = 0;
    uint8_t ep_out_ = 0;
    uint16_t ep_out_max_packet_ = 0;  // 0 on the fallback endpoints: unknown
    bool claimed_ = false;

    std::mutex write_mutex_;
    uint16_t mux_tx_ = 0;
    uint16_t mux_rx_ = 0;

    std::mutex conns_mutex_;
    std::map<uint16_t, std::shared_ptr<MuxTcpConn>> conns_;
    uint16_t next_sport_ = 1;

    std::vector<uint8_t> rxbuf_;
    std::atomic<bool> run_{false};
    std::atomic<bool> reader_stopped_{false};
    std::atomic<bool> write_failed_{false};
    std::thread reader_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_MUXD_H_
