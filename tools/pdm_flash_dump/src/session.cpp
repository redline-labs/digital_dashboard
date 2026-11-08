#include "pdm_flash_dump/session.h"

#include <array>
#include <string>

#include <spdlog/fmt/fmt.h>

#include "pdm32/config/crc16.h"

namespace pdm_flash_dump {

namespace {

constexpr std::array<Range, 9> kSafeRanges{{
    {0x0040, 0x0440, "RAM-1"},
    {0x0462, 0x0500, "FLASH-2 (factory)"},
    {0x0580, 0x0980, "RAM-2"},
    {0x0980, 0x1B80, "FLASH-2"},
    {0x1E20, 0x8000, "FLASH-2 (config, cal)"},
    {0x8000, 0xFE00, "FLASH-1 (application, bootloader)"},
    {0xFE20, 0xFF80, "monitor ROM"},
    {0xFF80, 0xFF88, "FLASH-1 (block protection)"},
    {0xFF89, 0x10000, "FLASH-1 (vectors)"},
}};

// The discovery beacon's first two bytes (an extended frame): 03 11 serHi serLo rx tx counter.
constexpr uint8_t kBeaconTag0 = 0x03;
constexpr uint8_t kBeaconTag1 = 0x11;

std::string read_status(uint8_t status) {
    switch (status) {
        case 2:
            return "no such region";
        case 6:
            return "bad request";
        case 7:
            return "zero length";
        case 9:
            return "region locked";
        default:
            return "status " + std::to_string(status);
    }
}

}  // namespace

std::span<const Range> safe_ranges() {
    return kSafeRanges;
}

std::vector<Range> default_plan() {
    std::vector<Range> plan;
    for (const Range& range : kSafeRanges) {
        if (range.name != "monitor ROM") {
            plan.push_back(range);
        }
    }
    return plan;
}

bool is_safe(uint32_t address, uint32_t length) {
    const uint32_t end = address + length;
    for (const Range& range : kSafeRanges) {
        if (range.from <= address && end <= range.to) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------- ChannelLink

void ChannelLink::send(const helpers::CanFrame& frame) {
    if (auto sent = channel_->send(frame); !sent) {
        throw ProtocolError("cannot send: " + sent.error().message);
    }
}

std::optional<helpers::CanFrame> ChannelLink::receive(Clock::time_point deadline) {
    while (pending_.empty()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            return std::nullopt;
        }
        std::array<helpers::CanFrame, 32> batch;
        const auto wait = std::chrono::duration_cast<can::Duration>(deadline - now);
        auto count = channel_->receive(batch, std::max(wait, can::Duration{1}));
        if (!count) {
            throw ProtocolError("cannot receive: " + count.error().message);
        }
        pending_.insert(pending_.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(*count));
    }
    helpers::CanFrame frame = pending_.front();
    pending_.pop_front();
    return frame;
}

// ------------------------------------------------------------------------------------------------- Session

Session::Session(Link& link, std::optional<uint16_t> serial, std::chrono::milliseconds timeout)
    : link_(link), serial_(serial), timeout_(timeout) {
}

void Session::send(Request request, std::span<const uint8_t> payload) {
    if (payload.size() > 8) {
        throw ProtocolError("a request payload is at most 8 bytes");
    }
    helpers::CanFrame frame;
    frame.id = (uint32_t{rx_node_} << 4) | static_cast<uint32_t>(request);
    frame.len = static_cast<uint8_t>(payload.size());  // at most 8, checked above
    std::copy(payload.begin(), payload.end(), frame.data.begin());
    sent_.push_back(frame);
    link_.send(frame);
}

uint16_t Session::discover(std::chrono::milliseconds wait) {
    const auto deadline = Clock::now() + wait;
    while (const auto frame = link_.receive(deadline)) {
        if (frame->isExtended && frame->len >= 6 && frame->data[0] == kBeaconTag0 && frame->data[1] == kBeaconTag1) {
            serial_ = static_cast<uint16_t>((frame->data[2] << 8) | frame->data[3]);
            rx_node_ = frame->data[4];
            tx_node_ = frame->data[5];
            return *serial_;
        }
    }
    throw ProtocolError(
        "no discovery beacon seen: give --serial (the unit beacons only with CAN transmission enabled in its "
        "configuration, or in its error mode)");
}

void Session::connect(uint8_t rx_node, uint8_t tx_node) {
    if (!serial_) {
        throw ProtocolError("no serial: discover() or --serial first");
    }
    if (rx_node == kDefaultRxNode || rx_node >= 0x80 || tx_node >= 0x80) {
        throw ProtocolError("session nodes must be below $80 and the rx node other than '?' ($3F)");
    }
    std::array<uint8_t, 8> body{0x00, 0x11, static_cast<uint8_t>(*serial_ >> 8), static_cast<uint8_t>(*serial_),
                                rx_node, tx_node, 0, 0};
    const uint16_t crc = pdm::config::crc16_ccitt(std::span<const uint8_t>(body.data(), 6));
    body[6] = static_cast<uint8_t>(crc >> 8);
    body[7] = static_cast<uint8_t>(crc);

    const uint8_t old_tx = tx_node_;
    // Before a session the unit listens on '?' whatever its beacon said.
    rx_node_ = kDefaultRxNode;
    send(Request::Connect, body);
    // The acknowledgement comes on the old tx node; then the unit switches.
    const auto reply = expect_response(old_tx, Request::Connect, Clock::now() + timeout_);
    if (reply.size() < 3 || reply[2] != 0) {
        throw ProtocolError("connect refused" + (reply.size() >= 3 ? " (status " + std::to_string(reply[2]) + ")"
                                                                    : std::string(" (short reply)")));
    }
    rx_node_ = rx_node;
    tx_node_ = tx_node;
}

std::vector<uint8_t> Session::expect_response(uint8_t tx_node, Request request, Clock::time_point deadline) {
    const uint32_t response_id = (uint32_t{tx_node} << 4) | 0x0F;
    while (const auto frame = link_.receive(deadline)) {
        if (!frame->isExtended && frame->id == response_id && frame->len > 0 &&
            frame->data[0] == static_cast<uint8_t>(request)) {
            const auto bytes = frame->data_span();
            return {bytes.begin(), bytes.end()};
        }
    }
    throw Stall(fmt::format("no response to request {} on ${:03X}", static_cast<int>(request), response_id));
}

std::vector<uint8_t> Session::read(uint16_t address, uint16_t length, uint8_t session_reload, int attempts) {
    for (int attempt = 1;; ++attempt) {
        try {
            return read_once(address, length, session_reload);
        } catch (const Stall&) {
            if (attempt >= attempts) {
                throw;
            }
            retries_++;
            // Let the unit's 50 ms flow-control timeout pass so it is idle again.
            drain(std::chrono::milliseconds(200));
        }
    }
}

void Session::drain(std::chrono::milliseconds period) {
    const auto deadline = Clock::now() + period;
    while (link_.receive(deadline)) {
    }
}

std::vector<uint8_t> Session::read_once(uint16_t address, uint16_t length, uint8_t session_reload) {
    if (length < 3) {
        throw ProtocolError("a read is 3..65535 bytes (the unit answers shorter ones without data)");
    }
    if (!is_safe(address, length)) {
        throw ProtocolError(fmt::format("${:04X}+{} is not RAM, flash or ROM: refused", address, length));
    }
    // {region 0, 0, offset, length, flags (bit 0: flow control), session reload (0: unchanged)}
    const std::array<uint8_t, 8> request{0,
                                         0,
                                         static_cast<uint8_t>(address >> 8),
                                         static_cast<uint8_t>(address),
                                         static_cast<uint8_t>(length >> 8),
                                         static_cast<uint8_t>(length),
                                         0x01,
                                         session_reload};
    send(Request::Read, request);

    std::vector<uint8_t> data;
    uint32_t expected_seq = 0;
    int in_block = 0;
    const uint32_t data_base = uint32_t{tx_node_} << 4;
    const uint32_t response_id = data_base | 0x0F;
    auto deadline = Clock::now() + timeout_;
    for (;;) {
        const auto frame = link_.receive(deadline);
        if (!frame) {
            throw Stall(fmt::format("read ${:04X}: timed out after {} of {} bytes", address, data.size(), length));
        }
        if (frame->isExtended) {
            continue;
        }
        if (frame->id == response_id && frame->len > 0 && frame->data[0] == static_cast<uint8_t>(Request::Read)) {
            if (frame->len < 3) {
                throw ProtocolError(fmt::format("read ${:04X}: a {}-byte response", address, frame->len));
            }
            if (frame->data[2] != 0) {
                throw ProtocolError(fmt::format("read ${:04X}: {}", address, read_status(frame->data[2])));
            }
            if (data.size() != length) {
                throw ProtocolError(fmt::format("read ${:04X}: {} bytes, {} asked", address, data.size(), length));
            }
            return data;
        }
        if (frame->id >= data_base && frame->id < data_base + 8) {
            const uint32_t seq = frame->id - data_base;
            if (seq != expected_seq) {
                throw ProtocolError(
                    fmt::format("read ${:04X}: frame {} out of order (expected {})", address, seq, expected_seq));
            }
            const auto bytes = frame->data_span();
            data.insert(data.end(), bytes.begin(), bytes.end());
            if (data.size() > length) {
                throw ProtocolError(fmt::format("read ${:04X}: {} bytes, more than the {} asked", address,
                                                data.size(), length));
            }
            expected_seq = (expected_seq + 1) & 7;
            deadline = Clock::now() + timeout_;
            // The unit waits for flow control after every 8th frame, also when the data is complete (its final
            // response comes after it); flow control at any other time would leave its comms busy.
            if (++in_block == 8) {
                in_block = 0;
                const std::array<uint8_t, 1> next{0};
                send(Request::Flow, next);
            }
        }
    }
}

void Session::close_session() {
    // The first read sets the session reload to 1, the second makes it the timeout; the unit then drops the
    // session at its next beacon.
    read(0x8000, 3, 1);
    read(0x8000, 3, 1);
}

}  // namespace pdm_flash_dump
