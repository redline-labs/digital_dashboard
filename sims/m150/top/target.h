// The M150 as the shared debugger sees it (host/debug/target.h): the e200's registers and memory by effective address
// (through the MMU, as the core sees them now), decoding from the engines' own decoder, the machine's pc and data
// watches, single steps through the interpreter, the PowerPC stack's back chain, snapshots of the core, memories and
// devices, symbols from the image's ELF files, and the board's own names:
//
//   can1.bit_rate  can1.sent  can1.received      a CAN port's controller (can1..can3: FlexCAN A..C)
//   interrupts  waiting                          interrupts taken since power-on; whether the core sleeps
#pragma once

#include "host/debug/target.h"
#include "m150/board/m150.h"

namespace sim::m150 {

class M150Target final : public sys::DebugTarget {
   public:
    M150Target(Board& board, sys::Symbols symbols) : b_(board), symbols_(std::move(symbols)) {
    }

    std::string cpu() const override {
        return "e200";
    }
    int address_digits() const override {
        return 8;
    }
    double seconds() const override;
    uint64_t now() const override;
    uint64_t ticks(double seconds) const override;

    uint64_t pc() const override;
    uint64_t stack_pointer() const override;
    nlohmann::json registers() const override;
    std::optional<uint64_t> read_register(const std::string& name) const override;
    bool write_register(const std::string& name, uint64_t value) override;

    bool peek(uint64_t address, uint8_t* out, size_t size) override;
    bool poke(uint64_t address, const uint8_t* in, size_t size) override;
    sys::Decoded decode(uint64_t address) override;

    void watch_pc(uint64_t pc, bool on) override;
    void skip_pc_once(uint64_t pc) override;
    int add_data_watch(uint64_t begin, uint64_t end, bool on_read, bool on_write, DataWatchFn fn) override;
    void remove_data_watch(int id) override;
    sys::TargetStop run_until(uint64_t tick) override;
    sys::TargetStop step_instruction(uint64_t max_ticks) override;
    bool stepped() const override {
        return stepped_;
    }
    void clear_exit() override;
    void reset() override;

    std::vector<uint64_t> return_addresses() override;

    std::any snapshot() override;
    void restore(const std::any& snapshot) override;

    const sys::Symbols& symbols() const override {
        return symbols_;
    }
    std::optional<double> read_device_name(const std::string& name, std::string& error) override;

   private:
    std::optional<uint32_t> word(uint32_t ea);
    bool after_call(uint32_t ret);

    Board& b_;
    sys::Symbols symbols_;
    bool stepped_ = false;
};

}  // namespace sim::m150
