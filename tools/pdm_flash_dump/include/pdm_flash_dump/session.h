// Reading a MoTeC PDM32's memory over CAN through the comms protocol PDM Manager uses, READ-ONLY by construction.
//
// Three requests exist here and no others: connect (type 9), read memory (type 14) and flow control (type 8). They
// are the only enumerators of Request, so an erase (10) or a write (13) cannot be spelled, not merely refused. A
// connect changes nothing but the unit's volatile comms state, which close_session() hands back; a read copies bytes
// out with plain loads, and only RAM, flash and the monitor ROM are read, because reading some peripheral registers
// clears status flags.
//
// The transport is a Link: any can::Channel (an adapter, a remote bus over zenoh:, the simulator) through
// ChannelLink, or a fake in a test.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "can/channel.h"
#include "helpers/can_frame.h"

namespace pdm_flash_dump {

using Clock = std::chrono::steady_clock;

class ProtocolError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

// No frame arrived in time. Session::read asks again after one of these; nothing else does.
class Stall : public ProtocolError {
   public:
    using ProtocolError::ProtocolError;
};

struct Range {
    uint16_t from;  // inclusive
    uint32_t to;    // exclusive
    std::string_view name;
};

// RAM, flash and the monitor ROM of the MC68HC908GZ60: everything else is a register window, or nothing.
std::span<const Range> safe_ranges();
// The same without the monitor ROM: what a dump reads unless asked for the ROM.
std::vector<Range> default_plan();
bool is_safe(uint32_t address, uint32_t length);

class Link {
   public:
    virtual ~Link() = default;
    virtual void send(const helpers::CanFrame& frame) = 0;
    // The next frame, or nothing by `deadline`.
    virtual std::optional<helpers::CanFrame> receive(Clock::time_point deadline) = 0;
};

class ChannelLink final : public Link {
   public:
    explicit ChannelLink(std::shared_ptr<can::Channel> channel) : channel_(std::move(channel)) {
    }
    void send(const helpers::CanFrame& frame) override;
    std::optional<helpers::CanFrame> receive(Clock::time_point deadline) override;

   private:
    std::shared_ptr<can::Channel> channel_;
    std::deque<helpers::CanFrame> pending_;
};

class Session {
   public:
    static constexpr uint8_t kDefaultRxNode = 0x3F;  // where the unit listens before a session: '?'
    static constexpr uint8_t kDefaultTxNode = 0x3E;
    static constexpr uint8_t kSessionRxNode = 0x3E;  // host -> unit on $3E0-$3EF during the session
    static constexpr uint8_t kSessionTxNode = 0x3F;  // unit -> host on $3F0-$3FF

    Session(Link& link, std::optional<uint16_t> serial, std::chrono::milliseconds timeout);

    // The unit's serial and comms nodes from its discovery beacon. Throws when none comes within `wait`.
    uint16_t discover(std::chrono::milliseconds wait);
    void connect(uint8_t rx_node = kSessionRxNode, uint8_t tx_node = kSessionTxNode);
    // `length` (3..65535) bytes from `address`, asked again after a stall: the unit drops a read whose flow control
    // is late, and a read has no side effects.
    std::vector<uint8_t> read(uint16_t address, uint16_t length, uint8_t session_reload = 0, int attempts = 4);
    // The unit times the session out and returns to its default nodes within about a second.
    void close_session();

    std::optional<uint16_t> serial() const {
        return serial_;
    }
    const std::vector<helpers::CanFrame>& sent() const {
        return sent_;
    }
    int retries() const {
        return retries_;
    }

   private:
    // Every request this tool can make. Nothing else can be sent.
    enum class Request : uint8_t {
        Flow = 8,
        Connect = 9,
        Read = 14,
    };

    void send(Request request, std::span<const uint8_t> payload);
    std::vector<uint8_t> expect_response(uint8_t tx_node, Request request, Clock::time_point deadline);
    std::vector<uint8_t> read_once(uint16_t address, uint16_t length, uint8_t session_reload);
    void drain(std::chrono::milliseconds period);

    Link& link_;
    std::optional<uint16_t> serial_;
    std::chrono::milliseconds timeout_;
    uint8_t rx_node_ = kDefaultRxNode;
    uint8_t tx_node_ = kDefaultTxNode;
    std::vector<helpers::CanFrame> sent_;
    int retries_ = 0;
};

}  // namespace pdm_flash_dump
