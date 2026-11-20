// The PDM as the shared debugger sees it (host/debug/target.h): the CPU08's registers and memory, decoding from the
// engines' own tables, the machine's pc and data watches, single steps, the stack heuristic, snapshots of the whole
// instance, and the PDM's own names:
//
//   battery                   battery volts
//   out[N].pin .v .i .tj .shutdown .load .status   output N (1-32): gate, volts, amps, junction degC, thermal
//                             shutdown, load ohms (inf: open), firmware status byte
//   in[N].v  in[N].raw        input N (1-23): connector volts; the firmware's reading (0.2 V/bit)
//   can:SIGNAL  can_age:SIGNAL   the latest decoded value of a DBC signal from the PDM's stream; its age in seconds
//   DEVICE.REG                a device register without side effects (mscan.CTFLG, tim1.TSC, adc.ADRL, ...)
//
// Settable: in[N].v (a number, or "open"), out[N].load ("open", "short", "lamp:R", ohms), battery.
#pragma once

#include "host/debug/target.h"
#include "pdm32/top/instance.h"

namespace sim::sys {

class Pdm32Target final : public DebugTarget {
   public:
    explicit Pdm32Target(Instance& instance) : in_(instance) {
    }

    std::string cpu() const override {
        return "hc08";
    }
    int address_digits() const override {
        return 4;
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
    Decoded decode(uint64_t address) override;

    void watch_pc(uint64_t pc, bool on) override;
    void skip_pc_once(uint64_t pc) override;
    int add_data_watch(uint64_t begin, uint64_t end, bool on_read, bool on_write, DataWatchFn fn) override;
    void remove_data_watch(int id) override;
    TargetStop run_until(uint64_t tick) override;
    TargetStop step_instruction(uint64_t max_ticks) override;
    bool stepped() const override {
        return stepped_;
    }
    void clear_exit() override;
    void reset() override;

    std::vector<uint64_t> return_addresses() override;

    std::any snapshot() override;
    void restore(const std::any& snapshot) override;

    const Symbols& symbols() const override {
        return in_.symbols;
    }
    std::optional<double> read_device_name(const std::string& name, std::string& error) override;
    std::optional<std::string> write_device_name(const std::string& name, const nlohmann::json& value) override;

   private:
    Instance& in_;
    bool stepped_ = false;
};

}  // namespace sim::sys
