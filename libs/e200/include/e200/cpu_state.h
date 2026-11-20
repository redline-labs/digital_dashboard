// The e200's programmer-visible state (user and supervisor), the bits of its control registers, and the result of a
// run of the core.
//
// The core is the e200z6 as the MPC55xx carry it: 32-bit Book E with the e200 extensions (isel, wait, the MAS-register
// MMU) and the SPE's scalar single-precision instructions (efs*), whose operands live in the low 32 bits of the GPRs.
// The SPE's vector instructions and the GPRs' upper halves are not modelled (no firmware here uses them; a vector
// instruction decodes as illegal).
//
// Register bit masks are written as values (MSR_EE = 0x00008000), not in Book E's big-endian bit numbering.
#pragma once

#include <array>
#include <cstdint>


namespace sim::e200 {

// MSR
inline constexpr uint32_t kMsrSpe = 0x02000000;  // SPE available
inline constexpr uint32_t kMsrWe = 0x00040000;   // wait state enable
inline constexpr uint32_t kMsrCe = 0x00020000;   // critical interrupts enabled
inline constexpr uint32_t kMsrEe = 0x00008000;   // external (and decrementer) interrupts enabled
inline constexpr uint32_t kMsrPr = 0x00004000;   // problem (user) state
inline constexpr uint32_t kMsrFp = 0x00002000;
inline constexpr uint32_t kMsrMe = 0x00001000;
inline constexpr uint32_t kMsrFe0 = 0x00000800;
inline constexpr uint32_t kMsrDe = 0x00000200;
inline constexpr uint32_t kMsrFe1 = 0x00000100;
inline constexpr uint32_t kMsrIs = 0x00000020;  // instruction address space
inline constexpr uint32_t kMsrDs = 0x00000010;  // data address space
// What an interrupt clears (Book E 5.6): the critical classes clear CE and DE as well.
inline constexpr uint32_t kMsrClearedByInterrupt = kMsrSpe | kMsrWe | kMsrEe | kMsrPr | kMsrFp | kMsrFe0 | kMsrFe1 |
                                                   kMsrIs | kMsrDs;

// XER
inline constexpr uint32_t kXerSo = 0x80000000;
inline constexpr uint32_t kXerOv = 0x40000000;
inline constexpr uint32_t kXerCa = 0x20000000;

// ESR (why a program, data storage or alignment interrupt was taken)
inline constexpr uint32_t kEsrPil = 0x08000000;  // illegal instruction
inline constexpr uint32_t kEsrPpr = 0x04000000;  // privileged instruction
inline constexpr uint32_t kEsrPtr = 0x02000000;  // trap
inline constexpr uint32_t kEsrSt = 0x00800000;   // a store
inline constexpr uint32_t kEsrSpe = 0x00000080;  // an SPE instruction

// TCR / TSR
inline constexpr uint32_t kTcrDie = 0x04000000;  // decrementer interrupt enable
inline constexpr uint32_t kTcrAre = 0x00400000;  // decrementer auto-reload
inline constexpr uint32_t kTsrDis = 0x08000000;  // decrementer interrupt status

// HID0
inline constexpr uint32_t kHid0Tben = 0x00004000;  // the timebase counts

// SPEFSCR (the bits this model sets: the sticky invalid/divide-by-zero/overflow/underflow/inexact flags)
inline constexpr uint32_t kSpefscrFinvs = 0x00000800;
inline constexpr uint32_t kSpefscrFdbzs = 0x00000400;
inline constexpr uint32_t kSpefscrFunfs = 0x00000200;
inline constexpr uint32_t kSpefscrFovfs = 0x00000100;
inline constexpr uint32_t kSpefscrFxs = 0x00000080;
inline constexpr uint32_t kSpefscrFinv = 0x00000020;
inline constexpr uint32_t kSpefscrFdbz = 0x00000010;
inline constexpr uint32_t kSpefscrFunf = 0x00000008;
inline constexpr uint32_t kSpefscrFovf = 0x00000004;
inline constexpr uint32_t kSpefscrFx = 0x00000001;
inline constexpr uint32_t kSpefscrFrmc = 0x00000003;  // rounding mode: 0 nearest, 1 zero, 2 +inf, 3 -inf

// The interrupt vectors (IVORn): the offset of each class from IVPR.
enum class Vector : uint8_t {
    CriticalInput = 0,
    MachineCheck = 1,
    DataStorage = 2,
    InstructionStorage = 3,
    ExternalInput = 4,
    Alignment = 5,
    Program = 6,
    FloatUnavailable = 7,
    SystemCall = 8,
    ApUnavailable = 9,
    Decrementer = 10,
    FixedInterval = 11,
    Watchdog = 12,
    DataTlbError = 13,
    InstructionTlbError = 14,
    Debug = 15,
    SpeUnavailable = 32,
    SpeFloatData = 33,
    SpeFloatRound = 34,
};

// The SPR numbers the core implements.
namespace spr {
inline constexpr uint16_t kXer = 1, kLr = 8, kCtr = 9, kDec = 22, kSrr0 = 26, kSrr1 = 27, kPid0 = 48, kDecar = 54,
                          kCsrr0 = 58, kCsrr1 = 59, kDear = 61, kEsr = 62, kIvpr = 63, kUsprg0 = 256, kSprg4R = 260,
                          kSprg5R = 261, kSprg6R = 262, kSprg7R = 263, kTbl = 268, kTbu = 269, kSprg0 = 272,
                          kSprg1 = 273, kSprg2 = 274, kSprg3 = 275, kSprg4 = 276, kSprg5 = 277, kSprg6 = 278,
                          kSprg7 = 279, kTblW = 284, kTbuW = 285, kPir = 286, kPvr = 287, kTsr = 336, kTcr = 340,
                          kIvor0 = 400, kIvor15 = 415, kIvor32 = 528, kIvor33 = 529, kIvor34 = 530, kSpefscr = 512,
                          kMcsrr0 = 570, kMcsrr1 = 571, kMcsr = 572, kMas0 = 624, kMas1 = 625, kMas2 = 626,
                          kMas3 = 627, kMas4 = 628, kMas6 = 630, kTlb0Cfg = 688, kTlb1Cfg = 689, kDsrr0 = 574,
                          kDsrr1 = 575, kHid0 = 1008, kHid1 = 1009, kL1Csr0 = 1010, kL1Csr1 = 1011, kMmuCsr0 = 1012,
                          kBucsr = 1013, kMmuCfg = 1015, kL1Cfg0 = 515, kL1Cfg1 = 516, kSvr = 1023;
}  // namespace spr

// A TLB1 entry, kept as the MAS1..MAS3 words tlbwe wrote (MAS1: V IPROT TID TS TSIZE; MAS2: EPN, VLE and WIMGE;
// MAS3: RPN, U0-U3 and the permissions UX SX UW SW UR SR).
struct TlbEntry {
    uint32_t mas1 = 0;
    uint32_t mas2 = 0;
    uint32_t mas3 = 0;

    bool valid() const {
        return (mas1 & 0x80000000) != 0;
    }
    bool iprot() const {
        return (mas1 & 0x40000000) != 0;
    }
    uint32_t tid() const {
        return (mas1 >> 16) & 0xFF;
    }
    uint32_t ts() const {
        return (mas1 >> 12) & 1;
    }
    // 4^TSIZE KB (TSIZE 1 is 4 KB, 5 is 1 MB, 10 is 1 GB)
    uint64_t size() const {
        return uint64_t(1024) << (2 * ((mas1 >> 8) & 0xF));
    }
    uint32_t mask() const {
        return uint32_t(~(size() - 1));
    }
    uint32_t epn() const {
        return mas2 & mask();
    }
    uint32_t rpn() const {
        return mas3 & mask();
    }
};

inline constexpr int kTlb1Entries = 32;

namespace perm {
inline constexpr uint32_t kUx = 0x20, kSx = 0x10, kUw = 0x08, kSw = 0x04, kUr = 0x02, kSr = 0x01;
}

struct CpuState {
    std::array<uint32_t, 32> gpr{};
    uint32_t pc = 0;
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t msr = 0;

    // supervisor
    uint32_t srr0 = 0, srr1 = 0, csrr0 = 0, csrr1 = 0, dsrr0 = 0, dsrr1 = 0, mcsrr0 = 0, mcsrr1 = 0, mcsr = 0;
    uint32_t dear = 0, esr = 0, ivpr = 0;
    std::array<uint32_t, 16> ivor{};      // IVOR0..15
    std::array<uint32_t, 3> ivor_spe{};   // IVOR32..34
    std::array<uint32_t, 8> sprg{};
    uint32_t usprg0 = 0;
    uint32_t pid0 = 0;
    uint32_t hid0 = 0, hid1 = 0, l1csr0 = 0, l1csr1 = 0, bucsr = 0;
    uint32_t tcr = 0, tsr = 0, dec = 0, decar = 0;
    uint32_t spefscr = 0;
    uint32_t pir = 0;

    // the MMU: TLB1 and the MAS registers tlbwe/tlbre/tlbsx use (MAS0..MAS4 and MAS6; index 5 is unused)
    std::array<TlbEntry, kTlb1Entries> tlb1{};
    std::array<uint32_t, 7> mas{};

    // the reservation lwarx takes and stwcx. checks
    bool reserved = false;
    uint32_t reservation = 0;

    // Core cycles since reset: the core's only clock (one per instruction; the machine converts them to ticks).
    uint64_t cycles = 0;
    // Instructions execute_insn has run: the interpreter's and the block engine's count, and the JIT's helper steps
    // (its native code does not count). The debugger single-steps through the interpreter, where it is exact.
    uint64_t instructions = 0;
    // The timebase at `tb_base_cycles` (it counts at the core clock while HID0[TBEN] is set).
    uint64_t tb_base = 0;
    uint64_t tb_base_cycles = 0;
    // The decrementer, `dec_base` at `dec_base_cycles`; it counts down with the timebase and stops at 0.
    uint32_t dec_base = 0;
    uint64_t dec_base_cycles = 0;
    // When the decrementer next reaches 0 (kNever: it is not counting). Kept by update_timer_event() (e200/cpu.h).
    uint64_t timer_event = ~uint64_t(0);

    // A breakpoint at this pc is passed once (the resume of a stop at a breakpoint).
    int64_t skip_watch_pc = -1;

    bool supervisor() const {
        return (msr & kMsrPr) == 0;
    }

    bool timebase_counts() const {
        return (hid0 & kHid0Tben) != 0;
    }
    uint64_t timebase() const {
        return timebase_counts() ? tb_base + (cycles - tb_base_cycles) : tb_base;
    }
    void set_timebase(uint64_t value) {
        tb_base = value;
        tb_base_cycles = cycles;
    }
    uint32_t decrementer() const {
        if (!timebase_counts()) {
            return dec_base;
        }
        const uint64_t elapsed = cycles - dec_base_cycles;
        return elapsed >= dec_base ? 0 : uint32_t(dec_base - elapsed);
    }
    void set_decrementer(uint32_t value) {
        dec_base = value;
        dec_base_cycles = cycles;
    }

    // CR field n (0..7): its 4 bits LT GT EQ SO in the low nibble
    uint32_t cr_field(unsigned n) const {
        return (cr >> (28 - 4 * n)) & 0xF;
    }
    void set_cr_field(unsigned n, uint32_t value) {
        const unsigned shift = 28 - 4 * n;
        cr = (cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
    }
    bool cr_bit(unsigned bit) const {
        return (cr >> (31 - bit)) & 1;
    }
    void set_cr_bit(unsigned bit, bool on) {
        const uint32_t mask = 1u << (31 - bit);
        cr = on ? (cr | mask) : (cr & ~mask);
    }

    uint32_t vector_address(Vector v) const {
        const unsigned n = unsigned(v);
        const uint32_t offset = n >= 32 ? ivor_spe[n - 32] : ivor[n];
        return (ivpr & 0xFFFF0000) | (offset & 0x0000FFF0);
    }
};

// Why the core returned. A stop is a value, never an exception.
enum class StopKind : uint8_t {
    None,     // (internal: the instruction completed)
    Limit,    // the cycle limit was reached (the quantum ended)
    Wait,     // wait executed, or MSR[WE] set: the core sleeps until an interrupt (pc is past it)
    Watch,    // a watched pc was reached (pc at it, not executed)
    Break,    // a data watch fired or a break was requested: the run stopped after the instruction
    Exit,     // request_exit(): the run was asked to end
    Checkstop,  // an interrupt the core cannot take (a machine check with MSR[ME] clear): the part stops
};

struct Stop {
    StopKind kind = StopKind::None;
    uint32_t pc = 0;
    uint32_t detail = 0;
};

}  // namespace sim::e200
