// SPDX-License-Identifier: GPL-3.0-or-later
//
// A CANopen device made of its own EDS.
//
// This could have been a table of canned replies, and it would have been about
// the same amount of code. It is not, because canned replies make every check
// worth making pass for free:
//
//   * Writes land, so reading a value back after writing it is a real test
//     rather than a tautology.
//   * Response command bytes come from the declared DataType, so an upload of
//     0x2010:02 answers 0x4B (two bytes) and 0x1800:02 answers 0x4F (one) --
//     the exact distinction that decides whether PDM Manager will accept a
//     keypad, and one a fixed-response stub would get right by accident or
//     wrong forever.
//   * Aborts are reachable. Write a read-only object, or a value outside its
//     declared limits, and a real abort code comes back to be handled.
//   * Non-volatile memory is modelled separately from live values, so "did the
//     configuration survive a reset" has an answer, and it is no by default
//     until something performs a Store.
//   * Node ID and bit rate change only on reset, and afterwards the device
//     answers at the new address and only at the new rate -- so a tool that
//     reorders the reconfiguration loses the device here rather than on a
//     bench.
//
// It is not a complete CANopen device. It implements what the reconfiguration
// touches: expedited and segmented SDO upload, expedited download, NMT, LSS,
// heartbeat and boot-up. PDO production is driven by the caller rather than by
// an event timer.
#ifndef CANOPEN_STUB_DEVICE_H
#define CANOPEN_STUB_DEVICE_H

#include "canopen/eds_ast.h"
#include "canopen/lss.h"
#include "canopen/nmt.h"
#include "canopen/virtual_bus.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace canopen
{

class StubDevice
{
public:
    // Values are seeded from the file's DefaultValues, with `$NODEID`
    // expressions resolved against `nodeId` -- which is what a device does at
    // reset, and means the COB-IDs are right without being listed here.
    StubDevice(VirtualBus& bus, ObjectDictionary od, uint8_t nodeId, LssBitrate bitrate);

    StubDevice(const StubDevice&) = delete;
    StubDevice& operator=(const StubDevice&) = delete;

    uint8_t node_id() const { return nodeId_; }
    LssBitrate bitrate() const { return bitrate_; }
    NmtState state() const { return state_; }

    // Live values, as an SDO client would see them.
    std::optional<uint64_t> value(uint16_t index, uint8_t sub) const;
    void set_value(uint16_t index, uint8_t sub, uint64_t value);

    // What survives a reset. Empty until a Store Parameters has been
    // performed, which is the whole point of having it separate.
    std::optional<uint64_t> stored_value(uint16_t index, uint8_t sub) const;
    bool has_stored_parameters() const { return !nonVolatile_.empty(); }

    // How many times the device has been reset, so a test can tell a reset
    // that happened from one that was merely commanded.
    uint32_t reset_count() const { return resetCount_; }

    // Emits a TPDO with the given payload, as the device would on a button
    // press. The COB-ID comes from the live 0x1800:01, so a reconfigured
    // COB-ID is reflected.
    void emit_tpdo1(std::span<const uint8_t> payload);

    // Makes an object refuse every write, whatever its declared AccessType.
    // Models firmware that is stricter than its own EDS -- which, per the
    // reverse-engineering, is precisely what a keypad that refuses
    // reconfiguration would look like.
    void make_read_only(uint16_t index, uint8_t sub);

    // How long the device takes to answer. Longer than the client's timeout
    // makes a timeout test.
    void set_response_delay(Duration delay) { responseDelay_ = delay; }

    // Silences the device entirely, for the "keypad not found" case.
    void set_present(bool present) { present_ = present; }

private:
    struct Entry
    {
        std::vector<uint8_t> bytes;
        DataType dataType { DataType::Unsigned8 };
        AccessType access { AccessType::RW };
        std::optional<int64_t> lowLimit;
        std::optional<int64_t> highLimit;
        bool forcedReadOnly { false };
    };

    using Key = std::pair<uint16_t, uint8_t>;

    void handle(const helpers::CanFrame& frame);
    void handle_sdo(const helpers::CanFrame& frame);
    void handle_nmt(const helpers::CanFrame& frame);
    void handle_lss(const helpers::CanFrame& frame);

    void reply(helpers::CanFrame frame);
    void abort(uint16_t index, uint8_t sub, uint32_t code);
    void reset();
    void seed_from_eds();

    Entry* find(uint16_t index, uint8_t sub);
    const Entry* find(uint16_t index, uint8_t sub) const;

    VirtualBus& bus_;
    ObjectDictionary od_;
    uint8_t nodeId_;
    LssBitrate bitrate_;
    NmtState state_ { NmtState::PreOperational };
    bool present_ { true };
    Duration responseDelay_ { 1 };
    uint32_t resetCount_ { 0 };

    std::map<Key, Entry> values_;
    std::map<Key, std::vector<uint8_t>> nonVolatile_;

    // LSS is a mode, not a message: outside configuration mode the device
    // ignores every LSS command but the one that enters it.
    bool lssConfiguration_ { false };
    // Written by LSS but not adopted until the next reset, which is what makes
    // "change addressing last" a rule rather than a suggestion.
    std::optional<uint8_t> pendingNodeId_;
    std::optional<LssBitrate> pendingBitrate_;
    std::optional<uint8_t> storedNodeId_;
    std::optional<LssBitrate> storedBitrate_;

    // A segmented upload in progress.
    struct Segmented
    {
        bool active { false };
        std::vector<uint8_t> bytes;
        size_t offset { 0 };
        bool toggle { false };
    };
    Segmented segmented_;
};

} // namespace canopen

#endif // CANOPEN_STUB_DEVICE_H
