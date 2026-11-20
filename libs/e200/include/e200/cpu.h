// The e200 core: the semantics of every instruction (execute_insn), interrupts and the MMU, and the run loop every
// engine shares (run_core).
//
// The environment is a template parameter, so memory accesses inline and the loop has no virtual calls. It must
// provide (duck-typed):
//
//   Fault    fetch(uint32_t ea, uint32_t* word)      an instruction word, through the MMU (no side effects)
//   Fault    load(uint32_t ea, unsigned size, uint32_t* value)   a big-endian data read of 1, 2 or 4 bytes, through
//                                                    the MMU; device registers may have side effects
//   Fault    store(uint32_t ea, unsigned size, uint32_t value)
//   void     translation_changed()                   the TLB, PID or MSR[PR/IS/DS] changed (drop cached translations)
//   uint64_t cycle_limit() const                     run until CpuState::cycles reaches it (re-read every instruction)
//   bool     external_pending() const                the external input (the interrupt controller's request) is high
//   bool     critical_pending() const                the critical input is high
//   bool     watching() const                        any pc watch is set (cheap test before pc_watched)
//   bool     pc_watched(uint32_t pc) const
//   Stop     on_interrupt(Vector, uint32_t pc)       told before every interrupt is taken; a Stop other than None
//                                                    ends the run after the entry (the debugger's catch)
//
// `Fault` says why an access did not happen; the core raises the interrupt it calls for (a TLB miss, a permission
// violation, a bus error) with the MMU's helpers below. The environment's MMU walk is mmu_translate().
//
// Timing: one cycle per instruction. The timebase and the decrementer count core cycles while HID0[TBEN] is set.
//
// Semantics follow the Book E PEM, the e200z6 Reference Manual (instruction set and interrupts) and the SPE PEM
// (e200/spe.h).
#pragma once

#include <bit>
#include <cstdint>

#include "e200/cpu_state.h"
#include "e200/isa.h"
#include "e200/spe.h"

namespace sim::e200 {

enum class Fault : uint8_t {
    None,
    TlbMiss,     // no TLB entry translates the address
    Protection,  // the entry forbids the access
    Bus,         // nothing answers at the physical address
};

enum class Access : uint8_t { Read, Write, Execute };

// --------------------------------------------------------------------------------------------------- the MMU

// The TLB1 entry that translates `ea` in address space `as` for process `pid`, or -1.
inline int mmu_lookup(const CpuState& s, uint32_t ea, uint32_t as, uint32_t pid) {
    for (int i = 0; i < kTlb1Entries; ++i) {
        const TlbEntry& e = s.tlb1[size_t(i)];
        if (e.valid() && e.ts() == as && (e.tid() == 0 || e.tid() == pid) && (ea & e.mask()) == e.epn()) {
            return i;
        }
    }
    return -1;
}

// EA -> PA for an access by the current MSR: the entry index (or -1 on a miss, Fault::TlbMiss), and Protection when
// the entry forbids it.
inline Fault mmu_translate(const CpuState& s, uint32_t ea, Access access, uint32_t* pa, int* entry = nullptr) {
    const uint32_t as = access == Access::Execute ? ((s.msr & kMsrIs) ? 1u : 0u) : ((s.msr & kMsrDs) ? 1u : 0u);
    const int i = mmu_lookup(s, ea, as, s.pid0 & 0xFF);
    if (entry) {
        *entry = i;
    }
    if (i < 0) {
        return Fault::TlbMiss;
    }
    const TlbEntry& e = s.tlb1[size_t(i)];
    const bool user = (s.msr & kMsrPr) != 0;
    uint32_t need = 0;
    switch (access) {
        case Access::Read:
            need = user ? perm::kUr : perm::kSr;
            break;
        case Access::Write:
            need = user ? perm::kUw : perm::kSw;
            break;
        case Access::Execute:
            need = user ? perm::kUx : perm::kSx;
            break;
    }
    *pa = e.rpn() | (ea & ~e.mask());
    return (e.mas3 & need) ? Fault::None : Fault::Protection;
}

// The reset state of TLB1: entry 0 maps the 4 KB boot page at 0xFFFFF000 to itself, for every access, protected.
inline void mmu_reset(CpuState& s) {
    s.tlb1 = {};
    s.mas = {};
    s.tlb1[0] = TlbEntry{0xC0000100, 0xFFFFF000, 0xFFFFF03F};
}

// -------------------------------------------------------------------------------------------- the timer

// Recomputes when the decrementer next reaches 0.
inline void update_timer_event(CpuState& s) {
    if (!s.timebase_counts() || s.dec_base == 0) {
        s.timer_event = ~uint64_t(0);
        return;
    }
    const uint64_t due = s.dec_base_cycles + s.dec_base;
    s.timer_event = due;
}

// The decrementer reached 0 at `s.timer_event`: TSR[DIS], and the auto-reload from DECAR.
inline void timer_expired(CpuState& s) {
    const uint64_t at = s.timer_event;
    s.tsr |= kTsrDis;
    if ((s.tcr & kTcrAre) && s.decar != 0) {
        s.dec_base = s.decar;
        s.dec_base_cycles = at;
    } else {
        s.dec_base = 0;
        s.dec_base_cycles = at;
    }
    update_timer_event(s);
}

namespace cpu_detail {

[[gnu::always_inline]] inline void set_cr0(CpuState& s, uint32_t r) {
    const int32_t v = int32_t(r);
    const uint32_t field = (v < 0 ? 8u : v > 0 ? 4u : 2u) | ((s.xer & kXerSo) ? 1u : 0u);
    s.cr = (s.cr & 0x0FFFFFFF) | (field << 28);
}

[[gnu::always_inline]] inline void set_ov(CpuState& s, bool ov) {
    if (ov) {
        s.xer |= kXerOv | kXerSo;
    } else {
        s.xer &= ~kXerOv;
    }
}

[[gnu::always_inline]] inline void set_ca(CpuState& s, bool ca) {
    s.xer = ca ? (s.xer | kXerCa) : (s.xer & ~kXerCa);
}

[[gnu::always_inline]] inline bool ca(const CpuState& s) {
    return (s.xer & kXerCa) != 0;
}

// a + b + c with the carry out and the signed overflow
[[gnu::always_inline]] inline uint32_t add_with(uint32_t a, uint32_t b, uint32_t c, bool* carry, bool* overflow) {
    const uint64_t sum = uint64_t(a) + b + c;
    const uint32_t r = uint32_t(sum);
    *carry = (sum >> 32) != 0;
    *overflow = ((a ^ r) & (b ^ r) & 0x80000000) != 0;
    return r;
}

// The rotate mask from MB to ME (Book E numbering), wrapping when MB > ME.
[[gnu::always_inline]] inline uint32_t rotate_mask(unsigned mb, unsigned me) {
    const uint32_t from_mb = 0xFFFFFFFFu >> mb;
    const uint32_t to_me = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (from_mb & to_me) : (from_mb | to_me);
}

[[gnu::always_inline]] inline uint32_t rotl(uint32_t v, unsigned n) {
    return std::rotl(v, int(n & 31));
}

[[gnu::always_inline]] inline uint32_t ra0(const CpuState& s, const Insn& in) {
    return in.a == 0 ? 0 : s.gpr[in.a];
}

inline uint32_t compare_field(const CpuState& s, bool lt, bool gt) {
    return (lt ? 8u : gt ? 4u : 2u) | ((s.xer & kXerSo) ? 1u : 0u);
}

inline bool trap_condition(unsigned to, uint32_t a, uint32_t b) {
    const int32_t sa = int32_t(a);
    const int32_t sb = int32_t(b);
    return ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) || ((to & 0x04) && a == b) || ((to & 0x02) && a < b) ||
           ((to & 0x01) && a > b);
}

// The SPRs a user-mode program may touch: those whose number has bit 0x10 clear.
inline bool spr_privileged(uint16_t spr) {
    return (spr & 0x10) != 0;
}

}  // namespace cpu_detail

// ---------------------------------------------------------------------------------------------- interrupts

// Takes an interrupt: the save/restore registers, the MSR (Book E 5.6), and the vector. `srr0` is where execution
// resumes after the handler's return (the faulting instruction, or the next one).
template <class Env>
inline Stop enter_interrupt(CpuState& s, Env& env, Vector v, uint32_t srr0) {
    const Stop stop = env.on_interrupt(v, srr0);
    const uint32_t old_msr = s.msr;
    if (v == Vector::CriticalInput || v == Vector::Watchdog) {
        s.csrr0 = srr0;
        s.csrr1 = old_msr;
        s.msr &= ~(kMsrClearedByInterrupt | kMsrCe | kMsrDe);
    } else if (v == Vector::Debug) {
        s.dsrr0 = srr0;
        s.dsrr1 = old_msr;
        s.msr &= ~(kMsrClearedByInterrupt | kMsrCe | kMsrDe);
    } else if (v == Vector::MachineCheck) {
        s.mcsrr0 = srr0;
        s.mcsrr1 = old_msr;
        s.msr &= ~(kMsrClearedByInterrupt | kMsrCe | kMsrDe | kMsrMe);
    } else {
        s.srr0 = srr0;
        s.srr1 = old_msr;
        s.msr &= ~kMsrClearedByInterrupt;
    }
    s.pc = s.vector_address(v);
    if ((old_msr ^ s.msr) & (kMsrPr | kMsrIs | kMsrDs)) {
        env.translation_changed();
    }
    return stop;
}

// A program interrupt for the instruction at `pc` with ESR = `esr` (illegal, privileged, trap).
template <class Env>
inline Stop program_interrupt(CpuState& s, Env& env, uint32_t pc, uint32_t esr) {
    s.esr = esr;
    return enter_interrupt(s, env, Vector::Program, pc);
}

// The interrupt a data access's fault calls for, at the instruction at `pc`.
template <class Env>
inline Stop data_fault(CpuState& s, Env& env, Fault fault, uint32_t pc, uint32_t ea, bool store) {
    s.dear = ea;
    s.esr = store ? kEsrSt : 0;
    switch (fault) {
        case Fault::TlbMiss: {
            // MAS registers loaded from MAS4's defaults, for the handler's tlbwe
            const uint32_t mas4 = s.mas[4];
            const uint32_t as = (s.msr & kMsrDs) ? 1u : 0u;
            s.mas[0] = (mas4 & 0x30000000) | ((s.mas[0] & 0x1F) << 16) | (s.mas[0] & 0x1F);
            s.mas[1] = 0x80000000 | ((s.pid0 & 0xFF) << 16) | (as << 12) | (mas4 & 0x00000F00);
            s.mas[2] = (ea & 0xFFFFF000) | (mas4 & 0x3F);
            s.mas[3] = 0;
            s.mas[6] = ((s.pid0 & 0xFF) << 16) | as;
            return enter_interrupt(s, env, Vector::DataTlbError, pc);
        }
        case Fault::Protection:
            return enter_interrupt(s, env, Vector::DataStorage, pc);
        case Fault::Bus:
        case Fault::None:
            break;
    }
    if (!(s.msr & kMsrMe)) {
        return Stop{StopKind::Checkstop, pc, ea};
    }
    s.mcsr = store ? 0x00000020 : 0x00000040;  // the bus error on a store / on a load
    return enter_interrupt(s, env, Vector::MachineCheck, pc);
}

template <class Env>
inline Stop fetch_fault(CpuState& s, Env& env, Fault fault, uint32_t pc) {
    switch (fault) {
        case Fault::TlbMiss: {
            const uint32_t mas4 = s.mas[4];
            const uint32_t as = (s.msr & kMsrIs) ? 1u : 0u;
            s.mas[0] = (mas4 & 0x30000000) | ((s.mas[0] & 0x1F) << 16) | (s.mas[0] & 0x1F);
            s.mas[1] = 0x80000000 | ((s.pid0 & 0xFF) << 16) | (as << 12) | (mas4 & 0x00000F00);
            s.mas[2] = (pc & 0xFFFFF000) | (mas4 & 0x3F);
            s.mas[3] = 0;
            s.mas[6] = ((s.pid0 & 0xFF) << 16) | as;
            return enter_interrupt(s, env, Vector::InstructionTlbError, pc);
        }
        case Fault::Protection:
            s.esr = 0;
            return enter_interrupt(s, env, Vector::InstructionStorage, pc);
        case Fault::Bus:
        case Fault::None:
            break;
    }
    if (!(s.msr & kMsrMe)) {
        return Stop{StopKind::Checkstop, pc, pc};
    }
    s.mcsr = 0x00000080;  // the bus error on a fetch
    return enter_interrupt(s, env, Vector::MachineCheck, pc);
}

// ------------------------------------------------------------------------------------------- SPRs, MSR

template <class Env>
inline void write_msr(CpuState& s, Env& env, uint32_t value) {
    const uint32_t old = s.msr;
    s.msr = value;
    if ((old ^ value) & (kMsrPr | kMsrIs | kMsrDs)) {
        env.translation_changed();
    }
}

// mfspr: false for an SPR the core does not implement
inline bool read_spr(const CpuState& s, uint16_t n, uint32_t* v) {
    switch (n) {
        case spr::kXer: *v = s.xer; return true;
        case spr::kLr: *v = s.lr; return true;
        case spr::kCtr: *v = s.ctr; return true;
        case spr::kDec: *v = s.decrementer(); return true;
        case spr::kSrr0: *v = s.srr0; return true;
        case spr::kSrr1: *v = s.srr1; return true;
        case spr::kPid0: *v = s.pid0; return true;
        case spr::kDecar: *v = s.decar; return true;
        case spr::kCsrr0: *v = s.csrr0; return true;
        case spr::kCsrr1: *v = s.csrr1; return true;
        case spr::kDsrr0: *v = s.dsrr0; return true;
        case spr::kDsrr1: *v = s.dsrr1; return true;
        case spr::kMcsrr0: *v = s.mcsrr0; return true;
        case spr::kMcsrr1: *v = s.mcsrr1; return true;
        case spr::kMcsr: *v = s.mcsr; return true;
        case spr::kDear: *v = s.dear; return true;
        case spr::kEsr: *v = s.esr; return true;
        case spr::kIvpr: *v = s.ivpr; return true;
        case spr::kUsprg0: *v = s.usprg0; return true;
        case spr::kTbl: *v = uint32_t(s.timebase()); return true;
        case spr::kTbu: *v = uint32_t(s.timebase() >> 32); return true;
        case spr::kPir: *v = s.pir; return true;
        case spr::kPvr: *v = 0x81150000; return true;  // e200z6
        case spr::kSvr: *v = 0; return true;
        case spr::kTsr: *v = s.tsr; return true;
        case spr::kTcr: *v = s.tcr; return true;
        case spr::kSpefscr: *v = s.spefscr; return true;
        case spr::kHid0: *v = s.hid0; return true;
        case spr::kHid1: *v = s.hid1; return true;
        case spr::kL1Csr0: *v = s.l1csr0; return true;
        case spr::kL1Csr1: *v = s.l1csr1; return true;
        case spr::kBucsr: *v = s.bucsr; return true;
        case spr::kL1Cfg0: *v = 0x00980B20; return true;  // 32 KB unified, 8-way, 32-byte lines
        case spr::kL1Cfg1: *v = 0; return true;
        case spr::kMmuCsr0: *v = 0; return true;
        case spr::kMmuCfg: *v = 0x00000184; return true;   // one TLB (TLB1), 8-bit PID
        case spr::kTlb0Cfg: *v = 0; return true;
        case spr::kTlb1Cfg: *v = 0x0B010000u | uint32_t(kTlb1Entries); return true;  // 4 KB .. 4 GB pages
        case spr::kMas0: *v = s.mas[0]; return true;
        case spr::kMas1: *v = s.mas[1]; return true;
        case spr::kMas2: *v = s.mas[2]; return true;
        case spr::kMas3: *v = s.mas[3]; return true;
        case spr::kMas4: *v = s.mas[4]; return true;
        case spr::kMas6: *v = s.mas[6]; return true;
        default: break;
    }
    if (n >= spr::kIvor0 && n <= spr::kIvor15) {
        *v = s.ivor[size_t(n - spr::kIvor0)];
        return true;
    }
    if (n >= spr::kIvor32 && n <= spr::kIvor34) {
        *v = s.ivor_spe[size_t(n - spr::kIvor32)];
        return true;
    }
    if (n >= spr::kSprg4R && n <= spr::kSprg7R) {
        *v = s.sprg[size_t(n - spr::kSprg4R + 4)];
        return true;
    }
    if (n >= spr::kSprg0 && n <= spr::kSprg7) {
        *v = s.sprg[size_t(n - spr::kSprg0)];
        return true;
    }
    return false;
}

// mtspr: false for an SPR the core does not implement (or one that is read-only)
template <class Env>
inline bool write_spr(CpuState& s, Env& env, uint16_t n, uint32_t v) {
    switch (n) {
        case spr::kXer: s.xer = v & 0xE000007F; return true;
        case spr::kLr: s.lr = v; return true;
        case spr::kCtr: s.ctr = v; return true;
        case spr::kDec:
            s.set_decrementer(v);
            update_timer_event(s);
            return true;
        case spr::kSrr0: s.srr0 = v; return true;
        case spr::kSrr1: s.srr1 = v; return true;
        case spr::kPid0:
            s.pid0 = v & 0xFF;
            env.translation_changed();
            return true;
        case spr::kDecar: s.decar = v; return true;
        case spr::kCsrr0: s.csrr0 = v; return true;
        case spr::kCsrr1: s.csrr1 = v; return true;
        case spr::kDsrr0: s.dsrr0 = v; return true;
        case spr::kDsrr1: s.dsrr1 = v; return true;
        case spr::kMcsrr0: s.mcsrr0 = v; return true;
        case spr::kMcsrr1: s.mcsrr1 = v; return true;
        case spr::kMcsr: s.mcsr &= ~v; return true;  // write 1 to clear
        case spr::kDear: s.dear = v; return true;
        case spr::kEsr: s.esr = v; return true;
        case spr::kIvpr: s.ivpr = v & 0xFFFF0000; return true;
        case spr::kUsprg0: s.usprg0 = v; return true;
        case spr::kTblW:
            s.set_timebase((s.timebase() & 0xFFFFFFFF00000000ull) | v);
            return true;
        case spr::kTbuW:
            s.set_timebase((s.timebase() & 0xFFFFFFFFull) | (uint64_t(v) << 32));
            return true;
        case spr::kPir: s.pir = v; return true;
        case spr::kTsr: s.tsr &= ~v; return true;  // write 1 to clear
        case spr::kTcr: s.tcr = v; return true;
        case spr::kSpefscr: s.spefscr = v; return true;
        case spr::kHid0: {
            // the timebase and decrementer freeze or resume at their present values
            const uint64_t tb = s.timebase();
            const uint32_t dec = s.decrementer();
            s.hid0 = v;
            s.set_timebase(tb);
            s.set_decrementer(dec);
            update_timer_event(s);
            return true;
        }
        case spr::kHid1: s.hid1 = v; return true;
        case spr::kL1Csr0: s.l1csr0 = v & ~0x00000006u; return true;  // CINV, CABT complete at once
        case spr::kL1Csr1: s.l1csr1 = v & ~0x00000006u; return true;
        case spr::kBucsr: s.bucsr = v & ~0x00000200u; return true;   // BBFI completes at once
        case spr::kMmuCsr0: return true;
        case spr::kMas0: s.mas[0] = v; return true;
        case spr::kMas1: s.mas[1] = v; return true;
        case spr::kMas2: s.mas[2] = v; return true;
        case spr::kMas3: s.mas[3] = v; return true;
        case spr::kMas4: s.mas[4] = v; return true;
        case spr::kMas6: s.mas[6] = v; return true;
        default: break;
    }
    if (n >= spr::kIvor0 && n <= spr::kIvor15) {
        s.ivor[size_t(n - spr::kIvor0)] = v & 0xFFF0;
        return true;
    }
    if (n >= spr::kIvor32 && n <= spr::kIvor34) {
        s.ivor_spe[size_t(n - spr::kIvor32)] = v & 0xFFF0;
        return true;
    }
    if (n >= spr::kSprg0 && n <= spr::kSprg7) {
        s.sprg[size_t(n - spr::kSprg0)] = v;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------ the TLB instructions

template <class Env>
inline void tlb_write(CpuState& s, Env& env) {
    if (((s.mas[0] >> 28) & 3) != 1) {
        return;  // TLB0 is not implemented: the write is ignored
    }
    TlbEntry& e = s.tlb1[(s.mas[0] >> 16) & (kTlb1Entries - 1)];
    e.mas1 = s.mas[1] & 0xC0FF1F00;
    e.mas2 = s.mas[2] & 0xFFFFF03F;
    e.mas3 = s.mas[3] & 0xFFFFF3FF;
    env.translation_changed();
}

inline void tlb_read(CpuState& s) {
    if (((s.mas[0] >> 28) & 3) != 1) {
        s.mas[1] = s.mas[2] = s.mas[3] = 0;
        return;
    }
    const TlbEntry& e = s.tlb1[(s.mas[0] >> 16) & (kTlb1Entries - 1)];
    s.mas[1] = e.mas1;
    s.mas[2] = e.mas2;
    s.mas[3] = e.mas3;
}

inline void tlb_search(CpuState& s, uint32_t ea) {
    const uint32_t pid = (s.mas[6] >> 16) & 0xFF;
    const uint32_t as = s.mas[6] & 1;
    const int i = mmu_lookup(s, ea, as, pid);
    if (i >= 0) {
        const TlbEntry& e = s.tlb1[size_t(i)];
        s.mas[0] = 0x10000000 | (uint32_t(i) << 16) | (s.mas[0] & 0x1F);
        s.mas[1] = e.mas1;
        s.mas[2] = e.mas2;
        s.mas[3] = e.mas3;
        return;
    }
    const uint32_t mas4 = s.mas[4];
    s.mas[0] = (mas4 & 0x30000000) | ((s.mas[0] & 0x1F) << 16) | (s.mas[0] & 0x1F);
    s.mas[1] = (pid << 16) | (as << 12) | (mas4 & 0x00000F00);
    s.mas[2] = (ea & 0xFFFFF000) | (mas4 & 0x3F);
    s.mas[3] = 0;
}

template <class Env>
inline void tlb_invalidate(CpuState& s, Env& env, uint32_t ea) {
    if (((ea >> 3) & 3) != 1) {
        return;  // only TLB1 exists
    }
    const bool all = (ea & 0x4) != 0;
    for (TlbEntry& e : s.tlb1) {
        if (e.valid() && !e.iprot() && (all || (ea & e.mask() & 0xFFFFF000) == (e.epn() & 0xFFFFF000))) {
            e.mas1 &= ~0x80000000u;
        }
    }
    env.translation_changed();
}

// ------------------------------------------------------------------------------------- instruction helpers

namespace cpu_detail {

[[gnu::always_inline]] inline uint32_t carry_in(const CpuState& s) {
    return (s.xer & kXerCa) ? 1u : 0u;
}

// The add/subtract family: rD = a + b + c, CA when `sets_ca`, OV when OE, CR0 when Rc.
[[gnu::always_inline]] inline void arith(CpuState& s, const Insn& in, uint32_t a, uint32_t b, uint32_t c,
                                         bool sets_ca) {
    bool carry, overflow;
    const uint32_t r = add_with(a, b, c, &carry, &overflow);
    if (sets_ca) {
        set_ca(s, carry);
    }
    if (in.has(flag::kOe)) {
        set_ov(s, overflow);
    }
    s.gpr[in.d] = r;
    if (in.has(flag::kRc)) {
        set_cr0(s, r);
    }
}

}  // namespace cpu_detail

// lbz .. lwzux: the update forms write the EA back to rA, after the access succeeded.
template <unsigned Size, bool Algebraic, bool Indexed, bool Update, class Env>
[[gnu::always_inline]] inline Stop load_op(CpuState& s, Env& env, const Insn& in, uint32_t pc) {
    const uint32_t base = Update ? s.gpr[in.a] : cpu_detail::ra0(s, in);
    const uint32_t ea = base + (Indexed ? s.gpr[in.b] : uint32_t(in.imm));
    uint32_t v = 0;
    const Fault f = env.load(ea, Size, &v);
    if (f != Fault::None) [[unlikely]] {
        return data_fault(s, env, f, pc, ea, false);
    }
    if constexpr (Algebraic) {
        v = uint32_t(int32_t(int16_t(uint16_t(v))));
    }
    if constexpr (Update) {
        s.gpr[in.a] = ea;
    }
    s.gpr[in.d] = v;
    s.pc = pc + 4;
    return Stop{};
}

template <unsigned Size, bool Indexed, bool Update, class Env>
[[gnu::always_inline]] inline Stop store_op(CpuState& s, Env& env, const Insn& in, uint32_t pc) {
    const uint32_t base = Update ? s.gpr[in.a] : cpu_detail::ra0(s, in);
    const uint32_t ea = base + (Indexed ? s.gpr[in.b] : uint32_t(in.imm));
    const Fault f = env.store(ea, Size, s.gpr[in.d]);
    if (f != Fault::None) [[unlikely]] {
        return data_fault(s, env, f, pc, ea, true);
    }
    if constexpr (Update) {
        s.gpr[in.a] = ea;
    }
    s.pc = pc + 4;
    return Stop{};
}

// ------------------------------------------------------------------------------------------- execute_insn

// Runs one instruction at s.pc. Returns StopKind::None to continue, or why the run must stop. An interrupt an
// instruction raises (a fault, sc, a trap, an illegal instruction) is taken here and is not a stop.
template <class Env>
inline Stop execute_insn(CpuState& s, Env& env, const Insn& in) {
    using namespace cpu_detail;
    const uint32_t pc = s.pc;
    uint32_t next = pc + 4;
    s.cycles += 1;
    s.instructions += 1;

    auto rd = [&](uint32_t v) { s.gpr[in.d] = v; };
    auto ra = [&](uint32_t v) {
        s.gpr[in.a] = v;
        if (in.has(flag::kRc)) {
            set_cr0(s, v);
        }
    };
    auto rd_rc = [&](uint32_t v) {
        s.gpr[in.d] = v;
        if (in.has(flag::kRc)) {
            set_cr0(s, v);
        }
    };
    auto privileged = [&]() { return (s.msr & kMsrPr) != 0; };

#define E200_LOAD(size, ea, out)                                         \
    do {                                                                 \
        const Fault f_ = env.load((ea), (size), &(out));                 \
        if (f_ != Fault::None) [[unlikely]] {                            \
            return data_fault(s, env, f_, pc, (ea), false);              \
        }                                                                \
    } while (0)
#define E200_STORE(size, ea, value)                                      \
    do {                                                                 \
        const Fault f_ = env.store((ea), (size), (value));               \
        if (f_ != Fault::None) [[unlikely]] {                            \
            return data_fault(s, env, f_, pc, (ea), true);               \
        }                                                                \
    } while (0)
#define E200_PRIVILEGED()                                                \
    do {                                                                 \
        if (privileged()) [[unlikely]] {                                 \
            return program_interrupt(s, env, pc, kEsrPpr);               \
        }                                                                \
    } while (0)

    switch (in.op) {
        case Op::Illegal:
        case Op::Count_:
            return program_interrupt(s, env, pc, kEsrPil);

        // ---- integer arithmetic
        case Op::Addi:
            rd(ra0(s, in) + uint32_t(in.imm));
            break;
        case Op::Addis:
            rd(ra0(s, in) + (uint32_t(in.imm) << 16));
            break;
        case Op::Addic:
        case Op::AddicRc: {
            bool c, o;
            const uint32_t r = add_with(s.gpr[in.a], uint32_t(in.imm), 0, &c, &o);
            set_ca(s, c);
            s.gpr[in.d] = r;
            if (in.op == Op::AddicRc) {
                set_cr0(s, r);
            }
            break;
        }
        case Op::Subfic: {
            bool c, o;
            set_ca(s, false);
            const uint32_t r = add_with(~s.gpr[in.a], uint32_t(in.imm), 1, &c, &o);
            set_ca(s, c);
            rd(r);
            break;
        }
        case Op::Mulli:
            rd(uint32_t(int32_t(s.gpr[in.a]) * in.imm));
            break;
        case Op::Add: arith(s, in, s.gpr[in.a], s.gpr[in.b], 0, false); break;
        case Op::Addc: arith(s, in, s.gpr[in.a], s.gpr[in.b], 0, true); break;
        case Op::Adde: arith(s, in, s.gpr[in.a], s.gpr[in.b], carry_in(s), true); break;
        case Op::Addme: arith(s, in, s.gpr[in.a], 0xFFFFFFFF, carry_in(s), true); break;
        case Op::Addze: arith(s, in, s.gpr[in.a], 0, carry_in(s), true); break;
        case Op::Subf: arith(s, in, ~s.gpr[in.a], s.gpr[in.b], 1, false); break;
        case Op::Subfc: arith(s, in, ~s.gpr[in.a], s.gpr[in.b], 1, true); break;
        case Op::Subfe: arith(s, in, ~s.gpr[in.a], s.gpr[in.b], carry_in(s), true); break;
        case Op::Subfme: arith(s, in, ~s.gpr[in.a], 0xFFFFFFFF, carry_in(s), true); break;
        case Op::Subfze: arith(s, in, ~s.gpr[in.a], 0, carry_in(s), true); break;
        case Op::Neg: arith(s, in, ~s.gpr[in.a], 0, 1, false); break;
        case Op::Mullw: {
            const int64_t p = int64_t(int32_t(s.gpr[in.a])) * int32_t(s.gpr[in.b]);
            if (in.has(flag::kOe)) {
                set_ov(s, p != int64_t(int32_t(p)));
            }
            rd_rc(uint32_t(p));
            break;
        }
        case Op::Mulhw:
            rd_rc(uint32_t(uint64_t(int64_t(int32_t(s.gpr[in.a])) * int32_t(s.gpr[in.b])) >> 32));
            break;
        case Op::Mulhwu:
            rd_rc(uint32_t((uint64_t(s.gpr[in.a]) * s.gpr[in.b]) >> 32));
            break;
        case Op::Divw: {
            const int32_t a = int32_t(s.gpr[in.a]);
            const int32_t b = int32_t(s.gpr[in.b]);
            const bool bad = b == 0 || (a == INT32_MIN && b == -1);
            // undefined in Book E; the e200 saturates
            const uint32_t r = bad ? (a < 0 && b == 0 ? 0x80000000u : 0x7FFFFFFFu) : uint32_t(a / b);
            if (in.has(flag::kOe)) {
                set_ov(s, bad);
            }
            rd_rc(r);
            break;
        }
        case Op::Divwu: {
            const uint32_t b = s.gpr[in.b];
            const uint32_t r = b == 0 ? 0xFFFFFFFFu : s.gpr[in.a] / b;
            if (in.has(flag::kOe)) {
                set_ov(s, b == 0);
            }
            rd_rc(r);
            break;
        }

        // ---- logical (rA = rS op ...)
        case Op::AndiRc:
            s.gpr[in.a] = s.gpr[in.d] & uint32_t(in.imm);
            set_cr0(s, s.gpr[in.a]);
            break;
        case Op::AndisRc:
            s.gpr[in.a] = s.gpr[in.d] & (uint32_t(in.imm) << 16);
            set_cr0(s, s.gpr[in.a]);
            break;
        case Op::Ori:
            s.gpr[in.a] = s.gpr[in.d] | uint32_t(in.imm);
            break;
        case Op::Oris:
            s.gpr[in.a] = s.gpr[in.d] | (uint32_t(in.imm) << 16);
            break;
        case Op::Xori:
            s.gpr[in.a] = s.gpr[in.d] ^ uint32_t(in.imm);
            break;
        case Op::Xoris:
            s.gpr[in.a] = s.gpr[in.d] ^ (uint32_t(in.imm) << 16);
            break;
        case Op::And: ra(s.gpr[in.d] & s.gpr[in.b]); break;
        case Op::Andc: ra(s.gpr[in.d] & ~s.gpr[in.b]); break;
        case Op::Or: ra(s.gpr[in.d] | s.gpr[in.b]); break;
        case Op::Orc: ra(s.gpr[in.d] | ~s.gpr[in.b]); break;
        case Op::Xor: ra(s.gpr[in.d] ^ s.gpr[in.b]); break;
        case Op::Nand: ra(~(s.gpr[in.d] & s.gpr[in.b])); break;
        case Op::Nor: ra(~(s.gpr[in.d] | s.gpr[in.b])); break;
        case Op::Eqv: ra(~(s.gpr[in.d] ^ s.gpr[in.b])); break;
        case Op::Extsb: ra(uint32_t(int32_t(int8_t(uint8_t(s.gpr[in.d]))))); break;
        case Op::Extsh: ra(uint32_t(int32_t(int16_t(uint16_t(s.gpr[in.d]))))); break;
        case Op::Cntlzw: ra(uint32_t(std::countl_zero(s.gpr[in.d]))); break;

        // ---- rotate and shift
        case Op::Rlwinm:
            ra(rotl(s.gpr[in.d], in.b) & rotate_mask(in.c, in.e));
            break;
        case Op::Rlwnm:
            ra(rotl(s.gpr[in.d], s.gpr[in.b] & 31) & rotate_mask(in.c, in.e));
            break;
        case Op::Rlwimi: {
            const uint32_t m = rotate_mask(in.c, in.e);
            ra((rotl(s.gpr[in.d], in.b) & m) | (s.gpr[in.a] & ~m));
            break;
        }
        case Op::Slw: {
            const uint32_t n = s.gpr[in.b] & 63;
            ra(n > 31 ? 0 : s.gpr[in.d] << n);
            break;
        }
        case Op::Srw: {
            const uint32_t n = s.gpr[in.b] & 63;
            ra(n > 31 ? 0 : s.gpr[in.d] >> n);
            break;
        }
        case Op::Sraw:
        case Op::Srawi: {
            const uint32_t n = in.op == Op::Srawi ? in.b : (s.gpr[in.b] & 63);
            const int32_t v = int32_t(s.gpr[in.d]);
            uint32_t r;
            bool carry;
            if (n > 31) {
                r = v < 0 ? 0xFFFFFFFFu : 0;
                carry = v < 0;
            } else {
                r = uint32_t(v >> n);
                carry = v < 0 && n > 0 && (uint32_t(v) & ((1u << n) - 1)) != 0;
            }
            set_ca(s, carry);
            ra(r);
            break;
        }

        // ---- compare
        case Op::Cmp:
            s.set_cr_field(in.d >> 2, compare_field(s, int32_t(s.gpr[in.a]) < int32_t(s.gpr[in.b]),
                                                    int32_t(s.gpr[in.a]) > int32_t(s.gpr[in.b])));
            break;
        case Op::Cmpi:
            s.set_cr_field(in.d >> 2,
                           compare_field(s, int32_t(s.gpr[in.a]) < in.imm, int32_t(s.gpr[in.a]) > in.imm));
            break;
        case Op::Cmpl:
            s.set_cr_field(in.d >> 2, compare_field(s, s.gpr[in.a] < s.gpr[in.b], s.gpr[in.a] > s.gpr[in.b]));
            break;
        case Op::Cmpli:
            s.set_cr_field(in.d >> 2,
                           compare_field(s, s.gpr[in.a] < uint32_t(in.imm), s.gpr[in.a] > uint32_t(in.imm)));
            break;

        // ---- branch
        case Op::B:
            next = in.has(flag::kAa) ? uint32_t(in.imm) : pc + uint32_t(in.imm);
            if (in.has(flag::kLk)) {
                s.lr = pc + 4;
            }
            break;
        case Op::Bc:
        case Op::Bclr:
        case Op::Bcctr: {
            const unsigned bo = in.d;
            bool ctr_ok = true;
            if (!(bo & 0x04) && in.op != Op::Bcctr) {
                s.ctr -= 1;
                ctr_ok = (s.ctr != 0) != ((bo & 0x02) != 0);
            }
            const bool cond_ok = (bo & 0x10) || (s.cr_bit(in.a) == ((bo & 0x08) != 0));
            const uint32_t target = in.op == Op::Bc    ? (in.has(flag::kAa) ? uint32_t(in.imm) : pc + uint32_t(in.imm))
                                    : in.op == Op::Bclr ? (s.lr & ~3u)
                                                        : (s.ctr & ~3u);
            if (in.has(flag::kLk)) {
                s.lr = pc + 4;
            }
            if (ctr_ok && cond_ok) {
                next = target;
            }
            break;
        }

        // ---- condition register
        case Op::Mcrf:
            s.set_cr_field(in.d >> 2, s.cr_field(in.a >> 2));
            break;
        case Op::Crand: s.set_cr_bit(in.d, s.cr_bit(in.a) && s.cr_bit(in.b)); break;
        case Op::Crandc: s.set_cr_bit(in.d, s.cr_bit(in.a) && !s.cr_bit(in.b)); break;
        case Op::Creqv: s.set_cr_bit(in.d, s.cr_bit(in.a) == s.cr_bit(in.b)); break;
        case Op::Crnand: s.set_cr_bit(in.d, !(s.cr_bit(in.a) && s.cr_bit(in.b))); break;
        case Op::Crnor: s.set_cr_bit(in.d, !(s.cr_bit(in.a) || s.cr_bit(in.b))); break;
        case Op::Cror: s.set_cr_bit(in.d, s.cr_bit(in.a) || s.cr_bit(in.b)); break;
        case Op::Crorc: s.set_cr_bit(in.d, s.cr_bit(in.a) || !s.cr_bit(in.b)); break;
        case Op::Crxor: s.set_cr_bit(in.d, s.cr_bit(in.a) != s.cr_bit(in.b)); break;
        case Op::Mfcr: rd(s.cr); break;
        case Op::Mtcrf: {
            uint32_t mask = 0;
            for (unsigned f = 0; f < 8; ++f) {
                if (uint32_t(in.imm) & (0x80u >> f)) {
                    mask |= 0xF0000000u >> (4 * f);
                }
            }
            s.cr = (s.cr & ~mask) | (s.gpr[in.d] & mask);
            break;
        }
        case Op::Mcrxr:
            s.set_cr_field(in.d >> 2, s.xer >> 28);
            s.xer &= 0x0FFFFFFF;
            break;
        case Op::Isel:
            rd(s.cr_bit(in.c) ? ra0(s, in) : s.gpr[in.b]);
            break;

        // ---- loads
        case Op::Lbz: return load_op<1, false, false, false>(s, env, in, pc);
        case Op::Lbzu: return load_op<1, false, false, true>(s, env, in, pc);
        case Op::Lbzx: return load_op<1, false, true, false>(s, env, in, pc);
        case Op::Lbzux: return load_op<1, false, true, true>(s, env, in, pc);
        case Op::Lhz: return load_op<2, false, false, false>(s, env, in, pc);
        case Op::Lhzu: return load_op<2, false, false, true>(s, env, in, pc);
        case Op::Lhzx: return load_op<2, false, true, false>(s, env, in, pc);
        case Op::Lhzux: return load_op<2, false, true, true>(s, env, in, pc);
        case Op::Lha: return load_op<2, true, false, false>(s, env, in, pc);
        case Op::Lhau: return load_op<2, true, false, true>(s, env, in, pc);
        case Op::Lhax: return load_op<2, true, true, false>(s, env, in, pc);
        case Op::Lhaux: return load_op<2, true, true, true>(s, env, in, pc);
        case Op::Lwz: return load_op<4, false, false, false>(s, env, in, pc);
        case Op::Lwzu: return load_op<4, false, false, true>(s, env, in, pc);
        case Op::Lwzx: return load_op<4, false, true, false>(s, env, in, pc);
        case Op::Lwzux: return load_op<4, false, true, true>(s, env, in, pc);
        case Op::Stb: return store_op<1, false, false>(s, env, in, pc);
        case Op::Stbu: return store_op<1, false, true>(s, env, in, pc);
        case Op::Stbx: return store_op<1, true, false>(s, env, in, pc);
        case Op::Stbux: return store_op<1, true, true>(s, env, in, pc);
        case Op::Sth: return store_op<2, false, false>(s, env, in, pc);
        case Op::Sthu: return store_op<2, false, true>(s, env, in, pc);
        case Op::Sthx: return store_op<2, true, false>(s, env, in, pc);
        case Op::Sthux: return store_op<2, true, true>(s, env, in, pc);
        case Op::Stw: return store_op<4, false, false>(s, env, in, pc);
        case Op::Stwu: return store_op<4, false, true>(s, env, in, pc);
        case Op::Stwx: return store_op<4, true, false>(s, env, in, pc);
        case Op::Stwux: return store_op<4, true, true>(s, env, in, pc);
        case Op::Lhbrx: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            uint32_t v = 0;
            E200_LOAD(2, ea, v);
            s.gpr[in.d] = ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
            break;
        }
        case Op::Lwbrx: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            uint32_t v = 0;
            E200_LOAD(4, ea, v);
            s.gpr[in.d] = std::byteswap(v);
            break;
        }
        case Op::Sthbrx: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            const uint32_t v = s.gpr[in.d];
            E200_STORE(2, ea, ((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
            break;
        }
        case Op::Stwbrx: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            E200_STORE(4, ea, std::byteswap(s.gpr[in.d]));
            break;
        }
        case Op::Lmw: {
            uint32_t ea = ra0(s, in) + uint32_t(in.imm);
            if (ea & 3) {
                s.dear = ea;
                s.esr = 0;
                return enter_interrupt(s, env, Vector::Alignment, pc);
            }
            for (unsigned r = in.d; r < 32; ++r, ea += 4) {
                uint32_t v = 0;
                E200_LOAD(4, ea, v);
                s.gpr[r] = v;
            }
            s.cycles += 31 - in.d;
            break;
        }
        case Op::Stmw: {
            uint32_t ea = ra0(s, in) + uint32_t(in.imm);
            if (ea & 3) {
                s.dear = ea;
                s.esr = kEsrSt;
                return enter_interrupt(s, env, Vector::Alignment, pc);
            }
            for (unsigned r = in.d; r < 32; ++r, ea += 4) {
                E200_STORE(4, ea, s.gpr[r]);
            }
            s.cycles += 31 - in.d;
            break;
        }
        case Op::Lwarx: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            if (ea & 3) {
                s.dear = ea;
                s.esr = 0;
                return enter_interrupt(s, env, Vector::Alignment, pc);
            }
            uint32_t v = 0;
            E200_LOAD(4, ea, v);
            s.gpr[in.d] = v;
            s.reserved = true;
            s.reservation = ea;
            break;
        }
        case Op::StwcxRc: {
            const uint32_t ea = ra0(s, in) + s.gpr[in.b];
            if (ea & 3) {
                s.dear = ea;
                s.esr = kEsrSt;
                return enter_interrupt(s, env, Vector::Alignment, pc);
            }
            const bool ok = s.reserved && s.reservation == ea;
            if (ok) {
                E200_STORE(4, ea, s.gpr[in.d]);
            }
            s.reserved = false;
            s.set_cr_field(0, (ok ? 2u : 0u) | ((s.xer & kXerSo) ? 1u : 0u));
            break;
        }
        case Op::Dcbz: {
            const uint32_t ea = (ra0(s, in) + s.gpr[in.b]) & ~31u;
            for (uint32_t i = 0; i < 32; i += 4) {
                E200_STORE(4, ea + i, 0);
            }
            break;
        }
        case Op::CacheNop:
        case Op::Msync:
        case Op::Mbar:
        case Op::Isync:
            break;

        // ---- system
        case Op::Sc:
            s.pc = next;
            return enter_interrupt(s, env, Vector::SystemCall, next);
        case Op::Tw:
        case Op::Twi: {
            const uint32_t b = in.op == Op::Twi ? uint32_t(in.imm) : s.gpr[in.b];
            if (trap_condition(in.d, s.gpr[in.a], b)) {
                return program_interrupt(s, env, pc, kEsrPtr);
            }
            break;
        }
        case Op::Mfspr: {
            if (spr_privileged(in.spr)) {
                E200_PRIVILEGED();
            }
            uint32_t v = 0;
            if (!read_spr(s, in.spr, &v)) {
                return program_interrupt(s, env, pc, kEsrPil);
            }
            s.gpr[in.d] = v;
            break;
        }
        case Op::Mtspr:
            if (spr_privileged(in.spr)) {
                E200_PRIVILEGED();
            }
            if (!write_spr(s, env, in.spr, s.gpr[in.d])) {
                return program_interrupt(s, env, pc, kEsrPil);
            }
            break;
        case Op::Mfmsr:
            E200_PRIVILEGED();
            s.gpr[in.d] = s.msr;
            break;
        case Op::Mtmsr:
            E200_PRIVILEGED();
            write_msr(s, env, s.gpr[in.d]);
            if (s.msr & kMsrWe) {
                s.pc = next;
                return Stop{StopKind::Wait, next, 0};
            }
            break;
        case Op::Wrtee:
            E200_PRIVILEGED();
            s.msr = (s.msr & ~kMsrEe) | (s.gpr[in.d] & kMsrEe);
            break;
        case Op::Wrteei:
            E200_PRIVILEGED();
            s.msr = in.imm ? (s.msr | kMsrEe) : (s.msr & ~kMsrEe);
            break;
        case Op::Rfi:
            E200_PRIVILEGED();
            write_msr(s, env, s.srr1);
            next = s.srr0 & ~3u;
            break;
        case Op::Rfci:
            E200_PRIVILEGED();
            write_msr(s, env, s.csrr1);
            next = s.csrr0 & ~3u;
            break;
        case Op::Rfdi:
            E200_PRIVILEGED();
            write_msr(s, env, s.dsrr1);
            next = s.dsrr0 & ~3u;
            break;
        case Op::Rfmci:
            E200_PRIVILEGED();
            write_msr(s, env, s.mcsrr1);
            next = s.mcsrr0 & ~3u;
            break;
        case Op::Wait:
            s.pc = next;
            return Stop{StopKind::Wait, next, 0};
        case Op::Tlbwe:
            E200_PRIVILEGED();
            tlb_write(s, env);
            break;
        case Op::Tlbre:
            E200_PRIVILEGED();
            tlb_read(s);
            break;
        case Op::Tlbsx:
            E200_PRIVILEGED();
            tlb_search(s, ra0(s, in) + s.gpr[in.b]);
            break;
        case Op::Tlbivax:
            E200_PRIVILEGED();
            tlb_invalidate(s, env, ra0(s, in) + s.gpr[in.b]);
            break;
        case Op::Tlbsync:
            E200_PRIVILEGED();
            break;

        // ---- SPE scalar single precision
        case Op::Efsadd: rd(spe::add(s.gpr[in.a], s.gpr[in.b], &s.spefscr)); break;
        case Op::Efssub: rd(spe::sub(s.gpr[in.a], s.gpr[in.b], &s.spefscr)); break;
        case Op::Efsmul: rd(spe::mul(s.gpr[in.a], s.gpr[in.b], &s.spefscr)); break;
        case Op::Efsdiv: rd(spe::div(s.gpr[in.a], s.gpr[in.b], &s.spefscr)); break;
        case Op::Efsabs: rd(s.gpr[in.a] & 0x7FFFFFFF); break;
        case Op::Efsnabs: rd(s.gpr[in.a] | 0x80000000); break;
        case Op::Efsneg: rd(s.gpr[in.a] ^ 0x80000000); break;
        // the compares set the GT bit of the field (the others are undefined; they are cleared)
        case Op::Efscmpgt: s.set_cr_field(in.d >> 2, spe::cmp_gt(s.gpr[in.a], s.gpr[in.b], &s.spefscr) ? 4 : 0); break;
        case Op::Efscmplt: s.set_cr_field(in.d >> 2, spe::cmp_lt(s.gpr[in.a], s.gpr[in.b], &s.spefscr) ? 4 : 0); break;
        case Op::Efscmpeq: s.set_cr_field(in.d >> 2, spe::cmp_eq(s.gpr[in.a], s.gpr[in.b], &s.spefscr) ? 4 : 0); break;
        case Op::Efststgt: s.set_cr_field(in.d >> 2, spe::tst_gt(s.gpr[in.a], s.gpr[in.b]) ? 4 : 0); break;
        case Op::Efststlt: s.set_cr_field(in.d >> 2, spe::tst_lt(s.gpr[in.a], s.gpr[in.b]) ? 4 : 0); break;
        case Op::Efststeq: s.set_cr_field(in.d >> 2, spe::tst_eq(s.gpr[in.a], s.gpr[in.b]) ? 4 : 0); break;
        case Op::Efscfsi: rd(spe::from_signed(int32_t(s.gpr[in.b]), &s.spefscr)); break;
        case Op::Efscfui: rd(spe::from_unsigned(s.gpr[in.b], &s.spefscr)); break;
        case Op::Efscfsf: rd(spe::from_signed_fraction(int32_t(s.gpr[in.b]), &s.spefscr)); break;
        case Op::Efscfuf: rd(spe::from_unsigned_fraction(s.gpr[in.b], &s.spefscr)); break;
        case Op::Efsctsi: rd(spe::to_signed(s.gpr[in.b], false, &s.spefscr)); break;
        case Op::Efsctui: rd(spe::to_unsigned(s.gpr[in.b], false, &s.spefscr)); break;
        case Op::Efsctsiz: rd(spe::to_signed(s.gpr[in.b], true, &s.spefscr)); break;
        case Op::Efsctuiz: rd(spe::to_unsigned(s.gpr[in.b], true, &s.spefscr)); break;
        case Op::Efsctsf: rd(spe::to_signed_fraction(s.gpr[in.b], &s.spefscr)); break;
        case Op::Efsctuf: rd(spe::to_unsigned_fraction(s.gpr[in.b], &s.spefscr)); break;
    }

#undef E200_LOAD
#undef E200_STORE
#undef E200_PRIVILEGED

    s.pc = next;
    return Stop{};
}

// ---------------------------------------------------------------------------------------------- run_core

// The interrupt the core takes now, if any, in priority order: critical input, external input, decrementer.
// True when an interrupt would be taken now (the wait state ends on one).
template <class Env>
inline bool interrupt_ready(const CpuState& s, const Env& env) {
    return ((s.msr & kMsrCe) && env.critical_pending()) ||
           ((s.msr & kMsrEe) && (env.external_pending() || ((s.tsr & kTsrDis) && (s.tcr & kTcrDie))));
}

// An interrupt's entry costs one cycle (as does the instruction that raised a synchronous one).
template <class Env>
[[gnu::always_inline]] inline bool take_pending_interrupt(CpuState& s, Env& env, Stop* stop) {
    if (!interrupt_ready(s, env)) {
        return false;
    }
    s.cycles += 1;
    if ((s.msr & kMsrCe) && env.critical_pending()) {
        *stop = enter_interrupt(s, env, Vector::CriticalInput, s.pc);
        return true;
    }
    if (s.msr & kMsrEe) {
        if (env.external_pending()) {
            *stop = enter_interrupt(s, env, Vector::ExternalInput, s.pc);
            return true;
        }
        if ((s.tsr & kTsrDis) && (s.tcr & kTcrDie)) {
            *stop = enter_interrupt(s, env, Vector::Decrementer, s.pc);
            return true;
        }
    }
    return false;
}

// The loop every engine shares: the cycle limit, the decrementer, interrupts, pc watches, then one instruction from
// `next_insn(s, pc, &insn)` (which returns a Fault for an instruction it could not fetch).
template <class Env, class NextInsn>
inline Stop run_core(CpuState& s, Env& env, NextInsn&& next_insn) {
    for (;;) {
        if (s.cycles >= s.timer_event) {
            timer_expired(s);
        }
        if (s.cycles >= env.cycle_limit()) {
            return Stop{StopKind::Limit, s.pc, 0};
        }

        Stop stop;
        if (take_pending_interrupt(s, env, &stop)) {
            if (stop.kind != StopKind::None) {
                return stop;
            }
            continue;
        }

        if (env.watching() && int64_t(s.pc) != s.skip_watch_pc && env.pc_watched(s.pc)) {
            return Stop{StopKind::Watch, s.pc, 0};
        }
        s.skip_watch_pc = -1;

        Insn in;
        const Fault fault = next_insn(s.pc, &in);
        if (fault != Fault::None) [[unlikely]] {
            s.cycles += 1;
            stop = fetch_fault(s, env, fault, s.pc);
        } else {
            stop = execute_insn(s, env, in);
        }
        if (stop.kind != StopKind::None) {
            return stop;
        }
    }
}

// The interpreter: decodes every instruction as it runs it.
template <class Env>
inline Stop interpret(CpuState& s, Env& env) {
    return run_core(s, env, [&env](uint32_t pc, Insn* in) {
        uint32_t word = 0;
        const Fault f = env.fetch(pc, &word);
        if (f == Fault::None) {
            *in = decode(word);
        }
        return f;
    });
}

// The state after a reset: supervisor, interrupts off, the boot page mapped, pc at the reset vector 0xFFFFFFFC.
inline void reset_core(CpuState& s) {
    s = CpuState{};
    mmu_reset(s);
    s.pc = 0xFFFFFFFC;
}

}  // namespace sim::e200
