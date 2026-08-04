// SPDX-License-Identifier: GPL-3.0-or-later
//
// Network management: telling a node to change state, and finding out what
// state it is in.
//
// The second half is the one that earns its keep. A reset takes an unknown
// amount of time and the device announces when it is finished by emitting a
// boot-up frame, so "reset and wait for boot-up" is a thing that can be waited
// for exactly rather than slept through. The reconfiguration sequence resets
// the keypad twice and re-reads objects afterwards; sleeping a guessed interval
// there is the difference between a tool that works and one that works most of
// the time.
#ifndef CANOPEN_NMT_H
#define CANOPEN_NMT_H

#include "canopen/bus.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace canopen
{

enum class NmtCommand : uint8_t
{
    Start = 0x01,
    Stop = 0x02,
    EnterPreOperational = 0x80,
    ResetNode = 0x81,
    ResetCommunication = 0x82,
};

const char* to_string(NmtCommand command);

// The state byte a node reports in its heartbeat. 0x00 is only ever seen once
// per reset: it is the boot-up message, not a steady state.
enum class NmtState : uint8_t
{
    BootUp = 0x00,
    Stopped = 0x04,
    Operational = 0x05,
    PreOperational = 0x7F,
};

const char* to_string(NmtState state);

// An emergency object. The device raises these on its own; nothing asks for
// them, and dropping them on the floor is how a device tells you about a fault
// and gets ignored.
struct EmcyMessage
{
    uint8_t nodeId { 0 };
    uint16_t errorCode { 0 };
    uint8_t errorRegister { 0 };
    std::array<uint8_t, 5> manufacturerSpecific {};
};

std::string to_string(const EmcyMessage& message);

// The frame an NMT command puts on the wire, so a dry run and an apply agree.
// Node 0 addresses every node.
helpers::CanFrame make_nmt_frame(NmtCommand command, uint8_t nodeId);

// Sends NMT commands and tracks what comes back on 0x700+id and 0x080+id.
//
// One instance watches the whole bus rather than a single node, because during
// an LSS reconfiguration the node ID is exactly the thing that changes.
class NmtMaster
{
public:
    explicit NmtMaster(Bus& bus);

    NmtMaster(const NmtMaster&) = delete;
    NmtMaster& operator=(const NmtMaster&) = delete;

    // Node 0 addresses every node on the bus. That is a real broadcast, not a
    // convenience: `stop(0)` is the first frame of the LSS sequence.
    void command(NmtCommand command, uint8_t nodeId);

    // The last state a node reported, or nothing if it has not been heard from.
    std::optional<NmtState> state(uint8_t nodeId) const;

    // Resets a node and waits for it to say it has finished. Returns false on
    // timeout, at which point the node is in an unknown state -- it may have
    // reset and failed to announce it, or never have received the command.
    bool reset_and_wait(uint8_t nodeId, Duration timeout = Duration { 5000 });

    // Waits for a boot-up frame without sending anything first, for the case
    // where the device was reset by other means (a power cycle, or an LSS
    // activate).
    bool wait_for_bootup(uint8_t nodeId, Duration timeout = Duration { 5000 });

    void on_emergency(std::function<void(const EmcyMessage&)> callback);
    void on_state_change(std::function<void(uint8_t, NmtState)> callback);

    static constexpr uint32_t kNmtCobId = 0x000;
    static constexpr uint32_t kHeartbeatCobIdBase = 0x700;
    static constexpr uint32_t kEmcyCobIdBase = 0x080;

private:
    Bus& bus_;
    std::map<uint8_t, NmtState> states_;
    // Counts boot-ups per node so a waiter can tell "it has booted since I
    // started waiting" from "it booted at some point in the past".
    std::map<uint8_t, uint32_t> bootCounts_;
    std::function<void(const EmcyMessage&)> emergencyCallback_;
    std::function<void(uint8_t, NmtState)> stateCallback_;
};

} // namespace canopen

#endif // CANOPEN_NMT_H
