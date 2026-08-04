// SPDX-License-Identifier: GPL-3.0-or-later
//
// LSS (CiA DS-305): changing a node's address and bit rate.
//
// This is the dangerous half of a reconfiguration and the interface says so.
// Switching to configuration mode is a *global* broadcast -- every LSS-capable
// device on the bus enters configuration mode, and the node ID that gets
// written lands on all of them. That is why MoTeC's own tool insists on a bus
// with nothing but the keypad and the gateway on it, and why nothing here will
// send a global switch unless the caller has explicitly asserted the bus is
// single-node.
//
// The other thing to know is the ordering. Node ID and bit rate persist through
// LSS Store, which is a different mechanism from the SDO 0x1010 "save" that
// persists everything else, and they take effect on the next reset. So LSS goes
// last in any sequence: change your own addressing before you are finished
// talking and you lose the device mid-flight, with nothing to recover it but a
// bit rate sweep.
#ifndef CANOPEN_LSS_H
#define CANOPEN_LSS_H

#include "canopen/bus.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace canopen
{

// The CiA 301 bit rate table, as the byte LSS wants. The Grayhill keypad ships
// at 250 kbit/s and MoTeC ships keypads at 1 Mbit/s, which is the single most
// common reason a keypad "cannot be found".
enum class LssBitrate : uint8_t
{
    Rate1000k = 0,
    Rate800k = 1,
    Rate500k = 2,
    Rate250k = 3,
    Rate125k = 4,
    Rate100k = 5,
    Rate50k = 6,
    Rate20k = 7,
    // 10 kbit/s is table index 8 in CiA 301 but this device does not support
    // it, and its EDS says so.
    Rate10k = 8,
};

// nullopt for a rate LSS has no encoding for.
std::optional<LssBitrate> lss_bitrate_from_kbps(uint32_t kbps);
uint32_t lss_bitrate_to_kbps(LssBitrate rate);
const char* to_string(LssBitrate rate);

struct LssError
{
    enum class Kind
    {
        // No LSS response. On a keypad-only bus this means the device is not
        // in configuration mode, or is not there at the assumed bit rate.
        Timeout,
        // The device answered with a non-zero error code.
        Rejected,
        BadResponse,
        // The caller asked for something this library will not do without the
        // single-node assertion.
        Refused,
    };

    Kind kind { Kind::Timeout };
    uint8_t errorCode { 0 };
    uint8_t specificError { 0 };
    std::string message;
};

std::string to_string(const LssError& error);

// The frame an LSS command puts on the wire, so a dry run and an apply agree.
helpers::CanFrame make_lss_frame(uint8_t commandSpecifier, uint8_t arg0, uint8_t arg1 = 0);

// The command specifiers, named. Exposed because a dry run has to describe a
// frame it is not going to send.
namespace lss_cs
{
inline constexpr uint8_t kSwitchStateGlobal = 0x04;
inline constexpr uint8_t kConfigureNodeId = 0x11;
inline constexpr uint8_t kConfigureBitTiming = 0x13;
inline constexpr uint8_t kActivateBitTiming = 0x15;
inline constexpr uint8_t kStoreConfiguration = 0x17;
inline constexpr uint8_t kInquireNodeId = 0x5E;

inline constexpr uint8_t kModeWaiting = 0x00;
inline constexpr uint8_t kModeConfiguration = 0x01;
// Byte 1 of a bit-timing frame selects the table; 0 is the CiA 301 standard
// table, the only one this device implements.
inline constexpr uint8_t kStandardBitTimingTable = 0x00;
} // namespace lss_cs

template <typename T>
using LssResult = std::expected<T, LssError>;

class LssMaster
{
public:
    // `singleNodeBus` is the caller asserting that nothing else on this bus
    // will answer a global LSS broadcast. Without it, every call that would
    // broadcast refuses.
    LssMaster(Bus& bus, bool singleNodeBus, Duration timeout = Duration { 1000 });

    LssMaster(const LssMaster&) = delete;
    LssMaster& operator=(const LssMaster&) = delete;

    Duration timeout() const { return timeout_; }
    void set_timeout(Duration timeout) { timeout_ = timeout; }

    bool single_node_bus() const { return singleNodeBus_; }

    // Puts every LSS-capable device on the bus into configuration mode. There
    // is no response to this frame -- LSS switch-state-global is unconfirmed,
    // so a device that did not hear it is indistinguishable from one that did
    // until the next command times out.
    LssResult<void> enter_configuration();
    LssResult<void> exit_configuration();

    // Both of these are confirmed. `configure_node_id` refuses an out-of-range
    // id locally rather than sending it.
    LssResult<void> configure_node_id(uint8_t nodeId);
    LssResult<void> configure_bitrate(LssBitrate rate);

    // Writes node ID and bit rate to non-volatile memory. Without this the
    // configuration is lost on the next power cycle.
    LssResult<void> store_configuration();

    // Tells devices to adopt the new bit timing after `delay` on each side of
    // the switch. Unconfirmed, and the bus is unusable during the switch.
    LssResult<void> activate_bitrate(Duration delay = Duration { 100 });

    // Reads back what the device is now configured as. Useful as the
    // read-after-write check for a node ID change, since after it the device
    // is only reachable at its new address.
    LssResult<uint8_t> inquire_node_id();

    void on_exchange(std::function<void(const std::string&)> callback);

    static constexpr uint32_t kMasterCobId = 0x7E5;
    static constexpr uint32_t kSlaveCobId = 0x7E4;

private:
    LssResult<void> send_unconfirmed(uint8_t cs, uint8_t arg0, uint8_t arg1 = 0);
    LssResult<helpers::CanFrame> send_confirmed(uint8_t cs, uint8_t arg0, uint8_t arg1 = 0);
    LssResult<void> require_single_node_bus(const char* what) const;
    void log(const std::string& line) const;

    Bus& bus_;
    bool singleNodeBus_;
    Duration timeout_;
    helpers::CanFrame pending_ {};
    bool pendingPresent_ { false };
    std::function<void(const std::string&)> exchangeCallback_;
};

} // namespace canopen

#endif // CANOPEN_LSS_H
