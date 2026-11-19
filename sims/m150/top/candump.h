// CAN in and out of a running M150 as candump text: a node that formats every frame on a bus ("(seconds) canN
// ID#DATA"), and the frames of a candump log sent onto the buses at their times.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emu/can_bus.h"
#include "emu/timed.h"
#include "m150/board/m150.h"

namespace sim::m150 {

// "(0.123456) can0 123#11223344"
std::string format_candump(const sys::CanFrame& frame, int bus, uint64_t now, uint64_t hz);

// Every frame on a bus, to `on_frame`.
class CanListener final : public sys::CanNode {
   public:
    CanListener(int bus, std::function<void(const sys::CanFrame&, int bus, uint64_t now)> on_frame)
        : bus_(bus), on_frame_(std::move(on_frame)) {
    }
    uint64_t frames = 0;
    bool receive(const sys::CanFrame& frame, const sys::CanNode&, uint64_t now) override {
        frames++;
        on_frame_(frame, bus_, now);
        return true;
    }

   private:
    int bus_;
    std::function<void(const sys::CanFrame&, int, uint64_t)> on_frame_;
};

// Frames sent at their times (an input: synced before the devices that receive them).
class CanInjector final : public sys::Timed, public sys::CanNode {
   public:
    struct Entry {
        uint64_t at;
        int bus;
        sys::CanFrame frame;
    };
    CanInjector(Board& board, std::vector<Entry> entries)
        : Timed("candump_in", sys::Timing::Input), board_(board), entries_(std::move(entries)) {
    }
    uint64_t next_event() const override {
        return next_ < entries_.size() ? entries_[next_].at : kNever;
    }
    void sync(uint64_t now) override {
        while (next_ < entries_.size() && entries_[next_].at <= now) {
            const Entry& e = entries_[next_++];
            board_.soc.can_bus[size_t(e.bus)].send(*this, e.frame, e.at);
        }
    }
    bool receive(const sys::CanFrame&, const sys::CanNode&, uint64_t) override {
        return false;
    }

   private:
    Board& board_;
    std::vector<Entry> entries_;
    size_t next_ = 0;
};

// A candump log's frames ("(t) canN ID#DATA"; canN is bus N, 0..2), the first at `start_s`, the rest at their
// offsets from it. Throws std::runtime_error when the file cannot be read.
std::vector<CanInjector::Entry> read_candump(const std::string& path, uint64_t hz, double start_s = 0.3);
std::vector<CanInjector::Entry> parse_candump(const std::string& text, uint64_t hz, double start_s = 0.3);

}  // namespace sim::m150
