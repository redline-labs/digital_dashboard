// The machine: the CPU, the address space, the devices and time, as one environment for the engines.
//
// Time is ticks of tick_hz (32 MHz, the GZ60's PLL VCO at the PDM's setting): a common multiple of every clock the
// part derives (bus 8 MHz = 4 ticks per cycle; 1 MHz from the crystal in the firmware's sleep; the ADC's and the
// timebase's dividers), so every rate divides exactly. The core counts bus cycles; now() converts.
//
// The run loop runs the CPU in quanta up to the earliest device event (no event queue: each Timed device reports
// next_event()). A device access that moves an event earlier ends the quantum after that instruction. At a quantum's
// end every due device is synced, Input devices first. A run cut into run_until() calls anywhere equals one uncut run.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hc08/irq.h"
#include "hc08/periph.h"
#include "hc08/jit/jit_engine.h"
#include "hc08/block_engine.h"
#include "hc08/cpu.h"
#include "hc08/cpu_state.h"
#include "hc08/engines.h"
#include "hc08/memmap.h"
#include "emu/irq_line.h"

namespace sim::sys {

struct MachineConfig {
    uint64_t tick_hz = 32'000'000;
    CpuEngine engine = kDefaultCpuEngine;
    // the longest quantum, in ticks (1 ms): bounds the latency of request_exit() and of the stimulus
    uint64_t max_quantum_ticks = 32'000;
};

// Why the machine reset (the SIM's SRSR bits).
enum class ResetCause : uint8_t { PowerOn = 0x80, Pin = 0x40, Cop = 0x20, IllegalOpcode = 0x10, IllegalAddress = 0x08 };

enum class CpuMode : uint8_t { Run, Wait, Stop };

class Machine final : public IrqSink {
   public:
    explicit Machine(MachineConfig config = {});

    MachineConfig cfg;
    MemoryMap mem;
    CpuState cpu;

    // ------------------------------------------------------------------ composition
    // Devices are added in order: the order of snapshots and of events due on the same tick.
    void add(std::shared_ptr<Periph> device);
    // maps [base, base + size) of the address space to `device` at offsets `offset`..
    void map(uint16_t base, uint32_t size, Periph& device, uint16_t offset = 0);

    template <class T>
    T* find() const {
        for (const std::shared_ptr<Periph>& p : devices_) {
            if (T* t = dynamic_cast<T*>(p.get())) {
                return t;
            }
        }
        return nullptr;
    }

    Periph* find(std::string_view name) const;

    const std::vector<std::shared_ptr<Periph>>& devices() const {
        return devices_;
    }

    // The flash controller takes the CPU's writes to flash (program/erase sequences).
    std::function<void(uint16_t addr, uint8_t value)> flash_write;

    // ------------------------------------------------------------------ time
    uint64_t now() const {
        return base_tick_ + (cpu.cycles - base_cycles_) * ticks_per_cycle_;
    }

    double seconds() const {
        return double(now()) / double(cfg.tick_hz);
    }

    uint64_t ticks_per_us() const {
        return cfg.tick_hz / 1'000'000;
    }

    uint32_t ticks_per_cycle() const {
        return ticks_per_cycle_;
    }

    uint64_t bus_hz() const {
        return cfg.tick_hz / ticks_per_cycle_;
    }

    // The bus clock changed (the CGM): every device is synced at the old rate, then told.
    void set_bus_hz(uint64_t hz);

    CpuMode mode() const {
        return mode_;
    }

    // ------------------------------------------------------------------ running
    // Runs until now() >= until (ticks), a watch or a requested exit. Returns StopKind::Limit when the time was reached.
    Stop run_until(uint64_t until);

    // Ask the run to end at the next quantum (any thread).
    void request_exit() {
        exit_requested_.store(true, std::memory_order_relaxed);
    }

    void clear_exit() {
        exit_requested_.store(false, std::memory_order_relaxed);
    }

    // A device's next event moved earlier outside an access to that device (another device's access changed it, as a
    // port write wakes the board's thermal stepping): end the quantum after this instruction if it falls inside it.
    void reschedule(uint64_t event) {
        if (event < quantum_end_) {
            limit_cycles_ = std::min(limit_cycles_, cpu.cycles);
        }
    }

    // Stop the run after the current instruction (from a device or hook on the machine's thread).
    void request_break() {
        break_requested_ = true;
        limit_cycles_ = cpu.cycles;
    }

    // A reset at the end of the current instruction (the COP, an illegal opcode). `on_reset` then re-boots the CPU.
    void request_reset(ResetCause cause);
    void reset(ResetCause cause);
    std::function<void(Machine&, ResetCause)> on_reset;
    uint64_t reset_count() const {
        return reset_count_;
    }

    // ------------------------------------------------------------------ debugging
    void watch_pc(uint16_t pc, bool on);
    void clear_pc_watches();

    struct DataWatch {
        uint16_t begin = 0;
        uint16_t end = 0;  // exclusive
        bool on_read = false;
        bool on_write = true;
        // returns true to stop the run after this instruction
        std::function<bool(uint16_t addr, uint8_t value, bool write)> fn;
    };
    int add_data_watch(DataWatch watch);
    void remove_data_watch(int id);

    // Memory as the debugger sees it: device registers through peek() (no side effects).
    uint8_t peek(uint16_t addr);
    // A debugger write: RAM and flash storage directly (the decode cache follows), devices through their write.
    void poke(uint16_t addr, uint8_t value);

    // ------------------------------------------------------------------ snapshots
    struct Snapshot {
        CpuState cpu;
        std::vector<uint8_t> memory;
        std::vector<uint8_t> devices;
        uint64_t base_tick = 0;
        uint64_t base_cycles = 0;
        uint32_t ticks_per_cycle = 4;
        uint32_t irq_lines = 0;
        CpuMode mode = CpuMode::Run;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);

    // ------------------------------------------------------------------ the engines' environment (hc08/cpu.h)
    [[gnu::always_inline]] uint8_t fetch(uint16_t addr) const {
        return mem.region(addr) == Region::Device ? 0 : mem.bytes[addr];
    }

    [[gnu::always_inline]] uint8_t read(uint16_t addr) {
        const Region region = mem.region(addr);
        uint8_t value;

        if (region == Region::Device) {
            value = device_read(addr);
        } else {
            value = mem.bytes[addr];
        }

        if (!data_watches_.empty()) {
            check_data_watch(addr, value, false);
        }
        return value;
    }

    [[gnu::always_inline]] void write(uint16_t addr, uint8_t value) {
        const Region region = mem.region(addr);

        if (region == Region::Ram) {
            mem.bytes[addr] = value;
            if (decode_cache_.has_code(addr)) {
                decode_cache_.invalidate(addr);
            }
        } else if (region == Region::Device) {
            device_write(addr, value);
        } else if (region == Region::Flash && flash_write) {
            flash_write(addr, value);
        }

        if (!data_watches_.empty()) {
            check_data_watch(addr, value, true);
        }
    }

    // Flash program/erase changed storage: drop decoded instructions there.
    void flash_changed(uint16_t addr, uint32_t size);

    uint64_t cycle_limit() const {
        return limit_cycles_;
    }

    bool irq_pending() const {
        return irq_lines_ != 0;
    }

    uint16_t irq_vector() const {
        return irq::vector_address(__builtin_ctz(irq_lines_));
    }

    bool watching() const {
        return pc_watch_count_ != 0;
    }

    bool pc_watched(uint16_t pc) const {
        return (pc_watch_[pc >> 6] >> (pc & 63)) & 1;
    }

    bool irq_pin() const {
        return true;  // the PDM ties IRQ high
    }

    // IrqSink: a device's line (source = irq::k*)
    void set_line(int source, bool on) override;

    uint32_t irq_lines() const {
        return irq_lines_;
    }

    DecodeCache& decode_cache() {
        return decode_cache_;
    }

    // the JIT's block cache (null until the Jit engine first runs)
    jit::JitCache* jit() const {
        return jit_.get();
    }

    // the JIT's view of memory (hc08/jit/jit_engine.h)
    uint8_t* jit_memory() {
        return mem.bytes.data();
    }
    const uint8_t* jit_regions() const {
        return mem.region_bytes();
    }
    const uint64_t* jit_limit() const {
        return &limit_cycles_;
    }
    const uint32_t* jit_irq_lines() const {
        return &irq_lines_;
    }

   private:
    uint8_t device_read(uint16_t addr);
    void device_write(uint16_t addr, uint8_t value);
    void after_device_access(Periph& device);
    void check_data_watch(uint16_t addr, uint8_t value, bool write);
    void sync_due(uint64_t t);
    uint64_t earliest_event() const;
    Stop run_engine();
    void enter_mode(CpuMode mode);

    std::vector<std::shared_ptr<Periph>> devices_;
    std::vector<Periph*> timed_;  // Input devices first, then Timed

    uint64_t base_tick_ = 0;
    uint64_t base_cycles_ = 0;
    uint32_t ticks_per_cycle_ = 4;
    uint64_t limit_cycles_ = 0;
    uint64_t quantum_end_ = 0;

    uint32_t irq_lines_ = 0;
    CpuMode mode_ = CpuMode::Run;

    std::atomic<bool> exit_requested_{false};
    bool break_requested_ = false;
    bool reset_pending_ = false;
    ResetCause reset_cause_ = ResetCause::PowerOn;
    uint64_t reset_count_ = 0;

    std::vector<uint64_t> pc_watch_ = std::vector<uint64_t>(1024, 0);
    int pc_watch_count_ = 0;
    std::vector<std::pair<int, DataWatch>> data_watches_;
    int next_watch_id_ = 1;

    DecodeCache decode_cache_;
    std::unique_ptr<jit::JitCache> jit_;
    bool jit_fallback_warned_ = false;
};

}  // namespace sim::sys
