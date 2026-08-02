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

namespace apple_usb
{

// Userspace CDC-NCM host for the NCM interface pair an iPhone exposes in the
// CarPlay configuration (bConfigurationValue 6). We take the pair away from the
// kernel's cdc_ncm driver, select the data altsetting, and bridge NTB16
// transfer blocks to a TAP device (cpusbN). The result is a point-to-point
// ethernet segment to the phone; an IPv6 link-local address (fe80::/64, EUI-64
// from the host MAC the phone dictates) is assigned to it and handed to the
// phone in CarPlayStartSession, which then opens TCP to [fe80::...]:7000.
//
// This class is Linux-only, but no longer because of USB: the USB side is
// libusb and builds anywhere, and the descriptor parsing it depends on lives in
// ncm_discovery.h, which is compiled and unit tested on every platform. What
// pins this file to Linux is the network plumbing -- /dev/net/tun, SIOCSIFHWADDR
// and /proc/sys/net/ipv6 have no macOS equivalent (utun is not TAP).
//
// Nothing here throws across the public API: failures are logged (every message
// is prefixed "[ncm]") and reported as false.
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
    // --- discovery ---
    //
    // The descriptor parsing this used to do by hand now lives in
    // ncm_discovery.h, which is portable and unit tested. What remains here is
    // the part that needs an open device: driver arbitration and the one
    // control transfer that reads the MAC string.

    // Releases any kernel driver holding this device's NCM interfaces. The
    // CarPlay configuration exposes two NCM pairs and cdc_ncm claims the first;
    // left alone, discovery would skip the driver-owned pair and silently
    // select the *second* one instead of the one LIVI uses.
    void detachKernelNcmDrivers();

    // Picks the NCM function to drive and fills ctrl_iface_/data_iface_ and the
    // endpoint fields from its descriptor.
    bool selectNcmFunction();

    // Reads the iMACAddress string descriptor named by the selected function.
    // Returns "" when unavailable. Needs an open handle but no claimed
    // interface -- it is a control transfer on endpoint 0.
    std::string readHostMac(uint8_t mac_string_index) const;

    // --- setup ---
    bool createTap();
    bool configureInterface(const std::string& mac);
    void cleanup();

    // --- pumps ---
    void statusLoop();
    void usbToTapLoop();
    void tapToUsbLoop();

    // The NTB16 framing these used to declare is now free functions in
    // ncm_frame.h -- portable, and unit tested by apple_usb_test_ncm_frame.

    DeviceInfo device_;
    DeviceHandle handle_;  // open USB handle for the phone
    int tap_fd_ = -1;      // /dev/net/tun fd owning the TAP device

    std::string host_mac_;

    // dwNtbInMaxSize from GET_NTB_PARAMETERS; echoed back in SET_NTB_INPUT_SIZE.
    uint32_t in_max_ = 32764;

    uint8_t ep_int_ = 0;
    std::thread status_thread_;

    uint8_t ctrl_iface_ = 0;
    uint8_t data_iface_ = 0;
    uint8_t mac_string_index_ = 0;
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
