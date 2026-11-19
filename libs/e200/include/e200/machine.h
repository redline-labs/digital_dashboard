// The e200 machine: the core, its 32-bit physical bus with memories and device windows, the devices on a schedule,
// the interrupt inputs, and the engines that run it.
//
// Time is the core clock: one tick per core cycle (MachineConfig::core_hz), and the core runs one instruction per
// cycle. The machine runs the core in quanta that end at the next device event (emu::Schedule), the requested time,
// or max_quantum_cycles, whichever is first; a device access mid-quantum syncs the device to the core's cycle first,
// and an access that moves the device's next event earlier ends the quantum there.
//
// The bus maps 4 KB physical pages to RAM, ROM (read directly; writes go to the ROM's controller device, if any) or a
// device window. Translation is the core's MMU (e200/cpu.h mmu_translate) behind a software TLB: per access kind,
// 256 direct-mapped entries holding the host address of a 4 KB page of RAM or ROM, one set per MSR mode (PR and the
// address space), dropped when the TLB or the PID changes. Device pages never enter it, nor do the pages of code a
// engine has decoded (writes there go the slow way, which drops the decoded code first).
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "emu/timed.h"
#include "e200/cpu.h"
#include "e200/cpu_state.h"
#include "e200/isa.h"
#include "e200/jit.h"

namespace sim::e200 {

class Machine;
namespace jit {
class JitCache;
struct Context;
}

// The software TLB's entries (see Machine), at fixed offsets the JIT's generated code reads.
inline constexpr uint32_t kSoftInvalidTag = 1;
inline constexpr size_t kSoftEntries = 256;
struct SoftTlbEntry {
    uint32_t tag = kSoftInvalidTag;  // the effective page, or kSoftInvalidTag
    uint32_t phys = 0;               // the physical page
    uint8_t* host = nullptr;         // the host address of the page's first byte
};
struct SoftTlb {
    std::array<SoftTlbEntry, kSoftEntries> read{};
    std::array<SoftTlbEntry, kSoftEntries> write{};
    std::array<SoftTlbEntry, kSoftEntries> exec{};
};
static_assert(sizeof(SoftTlbEntry) == 16);

// A device with a register window on the physical bus. Accesses are of 1, 2 or 4 bytes at their offset in the
// window, big-endian values.
class BusDevice : public sys::Timed {
   public:
    explicit BusDevice(std::string name, sys::Timing timing = sys::Timing::Passive)
        : Timed(std::move(name), timing) {
    }

    virtual uint32_t read(uint32_t offset, unsigned size) = 0;
    virtual void write(uint32_t offset, unsigned size, uint32_t value) = 0;
    // The value without side effects: the debugger's view. Defaults to read().
    virtual uint32_t peek(uint32_t offset, unsigned size) {
        return read(offset, size);
    }

    // Set when the device is mapped.
    Machine* m = nullptr;
};

enum class Engine : uint8_t { Interpreter, Block, Jit };

struct MachineConfig {
    uint64_t core_hz = 125'000'000;
    uint64_t max_quantum_cycles = 12'500;  // 100 us at 125 MHz
    Engine engine = Engine::Block;
};

struct EngineStatistics {
    uint64_t decoded = 0;      // instructions decoded into the block engine's cache
    uint64_t invalidated = 0;  // code pages dropped because they were written
    uint64_t interrupts = 0;
};

class Machine {
   public:
    explicit Machine(MachineConfig config = {});
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    MachineConfig cfg;
    CpuState state;

    // ---- the bus. Bases and sizes are multiples of 4 KB.
    // RAM, zeroed; returns its storage.
    uint8_t* map_ram(uint32_t base, uint32_t size);
    // ROM (flash): read directly; writes go to `controller` (at the offset from `base`), or fault if none.
    uint8_t* map_rom(uint32_t base, uint32_t size, BusDevice* controller = nullptr);
    // A device's register window: offset 0 of the window is `offset` in the device.
    void map(uint32_t base, uint32_t size, BusDevice& device, uint32_t offset = 0);
    // The host address of physical `pa` in RAM or ROM (null for a device page or a hole).
    uint8_t* host(uint32_t pa);
    // Copies `bytes` into RAM/ROM at physical `pa` (a loader's write: no device sees it). False if it is not all there.
    bool load_image(uint32_t pa, const uint8_t* bytes, size_t size);
    // The ROM's contents changed behind the bus (a flash controller programmed it): drop decoded code there.
    void memory_changed(uint32_t pa, uint32_t size);

    // ---- devices
    void add(std::shared_ptr<sys::Timed> device);
    sys::Timed* find(std::string_view name) const;
    const std::vector<std::shared_ptr<sys::Timed>>& devices() const {
        return devices_;
    }

    // ---- time
    uint64_t now() const {
        return state.cycles;
    }
    uint64_t ticks_per_us() const {
        return cfg.core_hz / 1'000'000;
    }
    // A device's next event moved earlier: the running quantum ends there.
    void reschedule(uint64_t event) {
        if (event < limit_cycles_) {
            limit_cycles_ = event < state.cycles ? state.cycles : event;
        }
    }

    // ---- the interrupt inputs (the interrupt controller drives them)
    void set_external_input(bool on);
    void set_critical_input(bool on);
    bool external_input() const {
        return external_;
    }

    // ---- running
    // Reset: the core (reset_core), every device, the decoded code; memories keep their contents.
    void reset();
    // Runs until now() reaches `until` or a stop other than Limit or Wait.
    Stop run_until(uint64_t until);
    bool waiting() const {
        return waiting_;
    }
    void request_exit() {
        exit_requested_ = true;
        limit_cycles_ = 0;
    }
    void clear_exit() {
        exit_requested_ = false;
    }
    void request_break() {
        break_requested_ = true;
        limit_cycles_ = 0;
    }

    // pc watches (effective addresses)
    void watch_pc(uint32_t pc, bool on);
    void clear_pc_watches();
    // Interrupt vectors that stop the run when taken (the debugger's catch): a Break after the entry.
    void catch_vector(Vector v, bool on);

    // Debugger access by effective address, through the MMU as the core would see it now (no side effects for
    // memory; a device is peeked). False when it does not translate or nothing is there.
    bool peek(uint32_t ea, unsigned size, uint32_t* value);
    bool poke(uint32_t ea, unsigned size, uint32_t value);

    const EngineStatistics& stats() const {
        return stats_;
    }
    // the JIT's counters (null until the JIT has run)
    const jit::JitStatistics* jit_stats() const {
        return jit_ ? &jit_->stats : nullptr;
    }

    // ---- the core's environment (e200/cpu.h)
    [[gnu::always_inline]] Fault fetch(uint32_t ea, uint32_t* word) {
        const SoftTlbEntry& e = soft_->exec[(ea >> 12) & kSoftMask];
        if (e.tag == (ea & ~0xFFFu)) [[likely]] {
            const uint8_t* p = e.host + (ea & 0xFFC);
            *word = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
            return Fault::None;
        }
        return fetch_slow(ea, word);
    }
    [[gnu::always_inline]] Fault load(uint32_t ea, unsigned size, uint32_t* value) {
        const SoftTlbEntry& e = soft_->read[(ea >> 12) & kSoftMask];
        if (e.tag == (ea & ~0xFFFu) && (ea & 0xFFF) <= 0x1000 - size) [[likely]] {
            *value = load_be(e.host + (ea & 0xFFF), size);
            return Fault::None;
        }
        return load_slow(ea, size, value);
    }
    [[gnu::always_inline]] Fault store(uint32_t ea, unsigned size, uint32_t value) {
        const SoftTlbEntry& e = soft_->write[(ea >> 12) & kSoftMask];
        if (e.tag == (ea & ~0xFFFu) && (ea & 0xFFF) <= 0x1000 - size) [[likely]] {
            store_be(e.host + (ea & 0xFFF), size, value);
            if (state.reserved && (state.reservation & ~3u) == (ea & ~3u)) {
                state.reserved = false;
            }
            return Fault::None;
        }
        return store_slow(ea, size, value);
    }
    void translation_changed();
    uint64_t cycle_limit() const {
        return limit_cycles_;
    }
    bool external_pending() const {
        return external_;
    }
    bool critical_pending() const {
        return critical_;
    }
    bool watching() const {
        return !pc_watches_.empty();
    }
    bool pc_watched(uint32_t pc) const;
    Stop on_interrupt(Vector v, uint32_t pc);

   private:
    static constexpr uint32_t kSoftMask = kSoftEntries - 1;
    static constexpr uint32_t kInvalidTag = kSoftInvalidTag;

    struct Page {
        uint8_t* read = nullptr;   // RAM or ROM
        uint8_t* write = nullptr;  // RAM only
        BusDevice* device = nullptr;
        uint32_t device_offset = 0;  // the page's first byte's offset in the device
    };
    struct PageBlock {
        std::array<Page, 256> pages{};  // 1 MB
    };

    // The block engine's decode cache, by physical page.
    struct CodePage {
        std::array<Insn, 1024> insn{};
        std::bitset<1024> valid;
    };

    static uint32_t load_be(const uint8_t* p, unsigned size) {
        switch (size) {
            case 1:
                return p[0];
            case 2:
                return (uint32_t(p[0]) << 8) | p[1];
            default:
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
    }
    static void store_be(uint8_t* p, unsigned size, uint32_t v) {
        for (unsigned i = 0; i < size; ++i) {
            p[i] = uint8_t(v >> (8 * (size - 1 - i)));
        }
    }

    Page* page(uint32_t pa);
    Page& page_for_map(uint32_t pa);
    Fault fetch_slow(uint32_t ea, uint32_t* word);
    Fault load_slow(uint32_t ea, unsigned size, uint32_t* value);
    Fault store_slow(uint32_t ea, unsigned size, uint32_t value);
    // EA -> PA by the MMU, and the soft TLB entry filled when the page is memory (`fill` null: do not fill)
    Fault translate(uint32_t ea, Access access, uint32_t* pa, SoftTlbEntry* fill);
    uint32_t device_read(Page& p, uint32_t pa, unsigned size);
    void device_write(Page& p, uint32_t pa, unsigned size, uint32_t value);
    void after_device_access(BusDevice& device);
    void select_soft_tlb();
    void flush_soft_tlbs();
    void code_written(uint32_t pa);
    void drop_code_page(uint32_t page);

    Stop run_engine();
    Stop run_block();
    Stop run_jit();
    Fault exec_physical(uint32_t pc, uint32_t* pa);
    Fault next_insn(uint32_t pc, Insn* in);
    jit::Block* jit_block(uint32_t ea);
    static uint32_t jit_step(jit::Context* c, const Insn* insn, uint32_t pc, uint32_t k);

    std::array<std::unique_ptr<PageBlock>, 4096> blocks_{};
    std::vector<std::unique_ptr<uint8_t[]>> storage_;
    std::vector<std::shared_ptr<sys::Timed>> devices_;
    sys::Schedule schedule_;

    std::array<SoftTlb, 8> soft_tlbs_{};  // by MSR: PR, IS, DS
    SoftTlb* soft_ = &soft_tlbs_[0];
    std::array<TlbEntry, kTlb1Entries> tlb_seen_{};
    uint32_t pid_seen_ = 0;

    std::bitset<(size_t(1) << 20)> code_pages_;  // physical 4 KB pages with decoded code
    std::unordered_map<uint32_t, std::unique_ptr<CodePage>> code_;
    uint32_t last_code_page_ = ~0u;
    CodePage* last_code_ = nullptr;

    uint64_t limit_cycles_ = 0;
    bool external_ = false;
    bool critical_ = false;
    bool waiting_ = false;
    bool exit_requested_ = false;
    bool break_requested_ = false;
    std::vector<uint32_t> pc_watches_;
    uint64_t caught_vectors_ = 0;
    bool caught_ = false;
    EngineStatistics stats_;

    std::unique_ptr<jit::JitCache> jit_;
    jit::Context jit_ctx_;
    uint64_t jit_block_end_ = 0;  // where the running block would end: a helper leaves if the limit moved before it
    bool jit_dropped_ = false;    // code was dropped while a block ran
    Stop jit_stop_;
    bool jit_fallback_warned_ = false;
};

}  // namespace sim::e200
