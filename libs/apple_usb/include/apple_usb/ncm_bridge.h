// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/ncm_bridge.py
#ifndef APPLE_USB_NCM_BRIDGE_H_
#define APPLE_USB_NCM_BRIDGE_H_

#include "apple_usb/usb_device.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apple_usb
{

// Userspace CDC-NCM host for the NCM interface pair an iPhone exposes in the
// CarPlay configuration (bConfigurationValue 6). The kernel cdc_ncm driver
// refuses to bind these, so we claim them through usbfs, select the data
// altsetting, and bridge NTB16 transfer blocks to a TAP device (cpusbN). The
// result is a point-to-point ethernet segment to the phone; an IPv6
// link-local address (fe80::/64, EUI-64 from the host MAC the phone dictates)
// is assigned to it and handed to the phone in CarPlayStartSession, which then
// opens TCP to [fe80::...]:7000.
//
// Linux-only, like the rest of this library. Nothing here throws across the
// public API: failures are logged (every message is prefixed "[ncm]") and
// reported as false.
class NcmBridge
{
  public:
    explicit NcmBridge(DeviceInfo device);
    ~NcmBridge();

    NcmBridge(const NcmBridge&) = delete;
    NcmBridge& operator=(const NcmBridge&) = delete;

    // Claim the NCM pair, create and configure the TAP device, and start the
    // two pump threads. Returns false (after cleaning up) on any failure.
    bool start();

    // Stop the pumps, release the interfaces and destroy the TAP device.
    // Idempotent; also called from the destructor.
    void stop();

    bool running() const { return run_.load(); }

    // TAP interface carrying the CarPlay AV link, e.g. "cpusb0". Empty until
    // start() succeeds.
    const std::string& interfaceName() const { return ifname_; }

    // The MAC the TAP interface actually ended up with -- the phone dictates it
    // through the CDC Ethernet functional descriptor. Goes into
    // CarPlayStartSession as the accessory device identifier, and is the value
    // linkLocalAddress() is derived from.
    const std::string& hostMac() const { return host_mac_; }

    // The accessory-side IPv6 link-local address on that interface, e.g.
    // "fe80::5a:aabb:ccdd:eeff". This is what goes into CarPlayStartSession.
    const std::string& linkLocalAddress() const { return fe80_; }

    const std::string& serial() const { return device_.serial; }

  private:
    // --- discovery (sysfs + device descriptors) ---

    // Releases any kernel driver holding this device's NCM interfaces. The
    // CarPlay configuration exposes two NCM pairs and cdc_ncm claims the first.
    void detachKernelNcmDrivers() const;

    // True when the kernel cdc_ncm driver already owns a netdev under this
    // device; in that case there is nothing for us to do (and claiming would
    // fight the driver).
    bool kernelNcmBound() const;

    // First unbound communications(0x02)/NCM(0x0d) control interface of the
    // active configuration whose successor is a CDC-data(0x0a) interface.
    bool findNcmPair(unsigned& ctrl_if, unsigned& data_if) const;

    // MAC the host must use, from the CDC Ethernet functional descriptor
    // (iMACAddress string). Returns "" when unavailable.
    std::string parseHostMac(unsigned ctrl_if) const;

    // Interrupt IN endpoint of the control interface. CDC devices deliver
    // NETWORK_CONNECTION / CONNECTION_SPEED_CHANGE notifications there, and the
    // kernel's cdc_ncm keeps a URB queued on it at all times.
    bool findInterruptEndpoint(unsigned ctrl_if, uint8_t& ep_int) const;

    // Bulk endpoints of the data interface's *current* altsetting; call only
    // after USBDEVFS_SETINTERFACE has selected altsetting 1.
    bool findBulkEndpoints(unsigned data_if, uint8_t& ep_in, uint8_t& ep_out) const;

    // --- setup ---
    bool createTap();
    bool configureInterface(const std::string& mac);
    void cleanup();

    // --- pumps ---
    void statusLoop();
    void usbToTapLoop();
    void tapToUsbLoop();

    // NTB16 -> ethernet datagrams. Malformed blocks yield what could be
    // recovered and a warning naming the offending offset.
    std::vector<std::vector<uint8_t>> parseNtb(const std::vector<uint8_t>& ntb) const;

    // One ethernet frame -> a single-datagram NTB16 block.
    std::vector<uint8_t> buildNtb(const uint8_t* frame, size_t len);

    DeviceInfo device_;
    int fd_ = -1;      // usbfs fd for the phone
    int tap_fd_ = -1;  // /dev/net/tun fd owning the TAP device

    std::string host_mac_;

    // dwNtbInMaxSize from GET_NTB_PARAMETERS; echoed back in SET_NTB_INPUT_SIZE.
    uint32_t in_max_ = 32764;

    uint8_t ep_int_ = 0;
    std::thread status_thread_;

    unsigned ctrl_iface_ = 0;
    unsigned data_iface_ = 0;
    bool ctrl_claimed_ = false;
    bool data_claimed_ = false;

    uint8_t ep_in_ = 0;
    uint8_t ep_out_ = 0;

    // dwNtbOutMaxSize from GET_NTB_PARAMETERS, clamped; the largest NTB the
    // phone is willing to receive.
    uint32_t out_max_ = 2048;

    std::string ifname_;
    std::string fe80_;

    std::mutex write_mutex_;  // serialises bulk OUT and the sequence number
    uint16_t seq_ = 0;

    std::atomic<bool> run_{false};
    std::thread usb_to_tap_;
    std::thread tap_to_usb_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_NCM_BRIDGE_H_
