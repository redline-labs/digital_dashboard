// The CPU08 (68HC08) instruction set: the opcode map with addressing modes, lengths and cycle counts from the CPU08
// Reference Manual (CPU08RM Rev 4, section 7 "Instruction Set" and the opcode map, table A-1). Only the HC08 core is
// accepted: the HCS08 additions (BGND $82, LDHX/STHX/CPHX extended and indexed, $9EAE.. forms) decode as Illegal, as the
// GZ60 would take them (an illegal-opcode reset).
//
// The table is built at compile time; decode() turns the bytes at a pc into an Insn (8 bytes) that every engine runs
// with the same execute_insn() (hc08/cpu.h).
#pragma once

#include <array>
#include <cstdint>

#include "emu/common.h"

namespace sim {

enum class Op : uint8_t {
    Illegal,
    // bit manipulation
    Brset,
    Brclr,
    Bset,
    Bclr,
    // branches
    Bra,
    Brn,
    Bhi,
    Bls,
    Bcc,
    Bcs,
    Bne,
    Beq,
    Bhcc,
    Bhcs,
    Bpl,
    Bmi,
    Bmc,
    Bms,
    Bil,
    Bih,
    Bge,
    Blt,
    Bgt,
    Ble,
    Bsr,
    // read-modify-write (memory, A or X by mode)
    Neg,
    Com,
    Lsr,
    Ror,
    Asr,
    Lsl,
    Rol,
    Dec,
    Inc,
    Tst,
    Clr,
    // compare-and-branch, decrement-and-branch, move
    Cbeq,
    Dbnz,
    Mov,
    // 16-bit index
    Ldhx,
    Sthx,
    Cphx,
    Aix,
    Ais,
    // register / memory
    Sub,
    Cmp,
    Sbc,
    Cpx,
    And,
    Bit,
    Lda,
    Sta,
    Eor,
    Adc,
    Ora,
    Add,
    Jmp,
    Jsr,
    Ldx,
    Stx,
    // inherent
    Mul,
    Div,
    Nsa,
    Daa,
    Rti,
    Rts,
    Swi,
    Tap,
    Tpa,
    Pula,
    Psha,
    Pulx,
    Pshx,
    Pulh,
    Pshh,
    Clrh,
    Stop,
    Wait,
    Txs,
    Tsx,
    Tax,
    Txa,
    Clc,
    Sec,
    Cli,
    Sei,
    Rsp,
    Nop,
};

enum class Mode : uint8_t {
    None,     // Illegal
    Inh,      // no operand
    InhA,     // A is the operand (NEGA, CBEQA #, DBNZA)
    InhX,     // X is the operand (NEGX, CBEQX #, DBNZX)
    Imm,      // #opr8
    Imm16,    // #opr16 (LDHX, CPHX)
    Dir,      // opr8 (page 0)
    Ext,      // opr16
    Ix,       // ,X
    Ix1,      // opr8,X
    Ix2,      // opr16,X
    Sp1,      // opr8,SP
    Sp2,      // opr16,SP
    Rel,      // a branch: rel8
    IxPlus,   // ,X+ (CBEQ ,X+ / MOV ,X+,d: H:X incremented after)
    Ix1Plus,  // opr8,X+ (CBEQ)
    DirDir,   // MOV opr8,opr8
    DirIxP,   // MOV opr8,X+
    ImmDir,   // MOV #opr8,opr8
};

struct OpInfo {
    Op op = Op::Illegal;
    Mode mode = Mode::None;
    uint8_t length = 1;  // bytes, including the opcode (and the $9E prefix)
    uint8_t cycles = 1;  // bus cycles
};

// A decoded instruction. `b` are the bytes after the opcode (after the $9E prefix and its opcode for the SP forms);
// `bit` is the bit number of BRSET/BRCLR/BSET/BCLR.
struct Insn {
    Op op = Op::Illegal;
    Mode mode = Mode::None;
    uint8_t length = 1;
    uint8_t cycles = 1;
    uint8_t bit = 0;
    uint8_t b[3] = {0, 0, 0};

    // the 16-bit operand of an Ext / Ix2 / Sp2 / Imm16 form
    uint16_t word() const {
        return uint16_t((uint16_t(b[0]) << 8) | b[1]);
    }
};

static_assert(sizeof(Insn) == 8);

namespace isa_detail {

// The ALU rows $A0..$FF: one operation per low nibble, one mode per high nibble.
inline constexpr Op kAluOps[16] = {Op::Sub, Op::Cmp, Op::Sbc, Op::Cpx, Op::And, Op::Bit, Op::Lda, Op::Sta,
                                   Op::Eor, Op::Adc, Op::Ora, Op::Add, Op::Jmp, Op::Jsr, Op::Ldx, Op::Stx};

// read-modify-write rows $30/$40/$50/$60/$70 (and $9E6x): the operation by low nibble; Illegal marks the special
// columns 1, 2, 5, B, E filled in separately
inline constexpr Op kRmwOps[16] = {Op::Neg, Op::Illegal, Op::Illegal, Op::Com, Op::Lsr, Op::Illegal,
                                   Op::Ror, Op::Asr,     Op::Lsl,     Op::Rol, Op::Dec, Op::Illegal,
                                   Op::Inc, Op::Tst,     Op::Illegal, Op::Clr};

constexpr OpInfo info(Op op, Mode mode, uint8_t length, uint8_t cycles) {
    return OpInfo{op, mode, length, cycles};
}

constexpr std::array<OpInfo, 256> make_base() {
    std::array<OpInfo, 256> t{};

    // $00-$0F BRSET/BRCLR n, $10-$1F BSET/BCLR n
    for (size_t n = 0; n < 8; n++) {
        t[0x00 + 2 * n] = info(Op::Brset, Mode::Dir, 3, 5);
        t[0x01 + 2 * n] = info(Op::Brclr, Mode::Dir, 3, 5);
        t[0x10 + 2 * n] = info(Op::Bset, Mode::Dir, 2, 4);
        t[0x11 + 2 * n] = info(Op::Bclr, Mode::Dir, 2, 4);
    }

    // $20-$2F branches
    constexpr Op kBranches[16] = {Op::Bra,  Op::Brn,  Op::Bhi, Op::Bls, Op::Bcc, Op::Bcs, Op::Bne, Op::Beq,
                                  Op::Bhcc, Op::Bhcs, Op::Bpl, Op::Bmi, Op::Bmc, Op::Bms, Op::Bil, Op::Bih};
    for (size_t k = 0; k < 16; k++) {
        t[0x20 + k] = info(kBranches[k], Mode::Rel, 2, 3);
    }

    // read-modify-write rows
    for (size_t k = 0; k < 16; k++) {
        const Op op = kRmwOps[k];
        if (op == Op::Illegal) {
            continue;
        }

        const bool tst = op == Op::Tst;
        const bool clr = op == Op::Clr;
        t[0x30 + k] = info(op, Mode::Dir, 2, tst || clr ? 3 : 4);
        t[0x40 + k] = info(op, Mode::InhA, 1, 1);
        t[0x50 + k] = info(op, Mode::InhX, 1, 1);
        t[0x60 + k] = info(op, Mode::Ix1, 2, tst ? 4 : (clr ? 3 : 5));
        t[0x70 + k] = info(op, Mode::Ix, 1, tst || clr ? 2 : 3);
    }

    // the special columns of the RMW rows
    t[0x31] = info(Op::Cbeq, Mode::Dir, 3, 5);
    t[0x41] = info(Op::Cbeq, Mode::InhA, 3, 4);  // CBEQA #opr8,rel
    t[0x51] = info(Op::Cbeq, Mode::InhX, 3, 4);  // CBEQX #opr8,rel
    t[0x61] = info(Op::Cbeq, Mode::Ix1Plus, 3, 5);
    t[0x71] = info(Op::Cbeq, Mode::IxPlus, 2, 4);
    t[0x42] = info(Op::Mul, Mode::Inh, 1, 5);
    t[0x52] = info(Op::Div, Mode::Inh, 1, 7);
    t[0x62] = info(Op::Nsa, Mode::Inh, 1, 3);
    t[0x72] = info(Op::Daa, Mode::Inh, 1, 2);
    t[0x35] = info(Op::Sthx, Mode::Dir, 2, 4);
    t[0x45] = info(Op::Ldhx, Mode::Imm16, 3, 3);
    t[0x55] = info(Op::Ldhx, Mode::Dir, 2, 4);
    t[0x65] = info(Op::Cphx, Mode::Imm16, 3, 3);
    t[0x75] = info(Op::Cphx, Mode::Dir, 2, 4);
    t[0x3B] = info(Op::Dbnz, Mode::Dir, 3, 5);
    t[0x4B] = info(Op::Dbnz, Mode::InhA, 2, 3);
    t[0x5B] = info(Op::Dbnz, Mode::InhX, 2, 3);
    t[0x6B] = info(Op::Dbnz, Mode::Ix1, 3, 5);
    t[0x7B] = info(Op::Dbnz, Mode::Ix, 2, 4);
    t[0x4E] = info(Op::Mov, Mode::DirDir, 3, 5);
    t[0x5E] = info(Op::Mov, Mode::DirIxP, 2, 4);
    t[0x6E] = info(Op::Mov, Mode::ImmDir, 3, 4);
    t[0x7E] = info(Op::Mov, Mode::IxPlus, 2, 4);

    // $80-$9F inherent
    t[0x80] = info(Op::Rti, Mode::Inh, 1, 7);
    t[0x81] = info(Op::Rts, Mode::Inh, 1, 4);
    t[0x83] = info(Op::Swi, Mode::Inh, 1, 9);
    t[0x84] = info(Op::Tap, Mode::Inh, 1, 2);
    t[0x85] = info(Op::Tpa, Mode::Inh, 1, 1);
    t[0x86] = info(Op::Pula, Mode::Inh, 1, 2);
    t[0x87] = info(Op::Psha, Mode::Inh, 1, 2);
    t[0x88] = info(Op::Pulx, Mode::Inh, 1, 2);
    t[0x89] = info(Op::Pshx, Mode::Inh, 1, 2);
    t[0x8A] = info(Op::Pulh, Mode::Inh, 1, 2);
    t[0x8B] = info(Op::Pshh, Mode::Inh, 1, 2);
    t[0x8C] = info(Op::Clrh, Mode::Inh, 1, 1);
    t[0x8E] = info(Op::Stop, Mode::Inh, 1, 1);
    t[0x8F] = info(Op::Wait, Mode::Inh, 1, 1);
    t[0x90] = info(Op::Bge, Mode::Rel, 2, 3);
    t[0x91] = info(Op::Blt, Mode::Rel, 2, 3);
    t[0x92] = info(Op::Bgt, Mode::Rel, 2, 3);
    t[0x93] = info(Op::Ble, Mode::Rel, 2, 3);
    t[0x94] = info(Op::Txs, Mode::Inh, 1, 2);
    t[0x95] = info(Op::Tsx, Mode::Inh, 1, 2);
    t[0x97] = info(Op::Tax, Mode::Inh, 1, 1);
    t[0x98] = info(Op::Clc, Mode::Inh, 1, 1);
    t[0x99] = info(Op::Sec, Mode::Inh, 1, 1);
    t[0x9A] = info(Op::Cli, Mode::Inh, 1, 2);
    t[0x9B] = info(Op::Sei, Mode::Inh, 1, 2);
    t[0x9C] = info(Op::Rsp, Mode::Inh, 1, 1);
    t[0x9D] = info(Op::Nop, Mode::Inh, 1, 1);
    t[0x9F] = info(Op::Txa, Mode::Inh, 1, 1);

    // $A0-$FF register / memory: IMM, DIR, EXT, IX2, IX1, IX
    constexpr Mode kModes[6] = {Mode::Imm, Mode::Dir, Mode::Ext, Mode::Ix2, Mode::Ix1, Mode::Ix};
    constexpr uint8_t kLengths[6] = {2, 2, 3, 3, 2, 1};
    constexpr uint8_t kCycles[6] = {2, 3, 4, 4, 3, 2};
    for (size_t m = 0; m < 6; m++) {
        for (size_t k = 0; k < 16; k++) {
            t[0xA0 + 16 * m + k] = info(kAluOps[k], kModes[m], kLengths[m], kCycles[m]);
        }
    }

    // the immediate row's exceptions: no STA/JMP/JSR/STX immediate
    t[0xA7] = info(Op::Ais, Mode::Imm, 2, 2);
    t[0xAC] = OpInfo{};
    t[0xAD] = info(Op::Bsr, Mode::Rel, 2, 4);
    t[0xAF] = info(Op::Aix, Mode::Imm, 2, 2);

    // JMP and JSR take their own cycle counts
    t[0xBC] = info(Op::Jmp, Mode::Dir, 2, 2);
    t[0xCC] = info(Op::Jmp, Mode::Ext, 3, 3);
    t[0xDC] = info(Op::Jmp, Mode::Ix2, 3, 4);
    t[0xEC] = info(Op::Jmp, Mode::Ix1, 2, 3);
    t[0xFC] = info(Op::Jmp, Mode::Ix, 1, 2);
    t[0xBD] = info(Op::Jsr, Mode::Dir, 2, 4);
    t[0xCD] = info(Op::Jsr, Mode::Ext, 3, 5);
    t[0xDD] = info(Op::Jsr, Mode::Ix2, 3, 6);
    t[0xED] = info(Op::Jsr, Mode::Ix1, 2, 5);
    t[0xFD] = info(Op::Jsr, Mode::Ix, 1, 4);

    return t;
}

// The $9E page: the stack-pointer-relative forms of the RMW row ($9E6x) and of the register/memory rows ($9EDx, $9EEx).
constexpr std::array<OpInfo, 256> make_page9e() {
    std::array<OpInfo, 256> t{};

    for (size_t k = 0; k < 16; k++) {
        const Op op = kRmwOps[k];
        if (op != Op::Illegal) {
            t[0x60 + k] = info(op, Mode::Sp1, 3, op == Op::Tst ? 5 : (op == Op::Clr ? 4 : 6));
        }
    }

    t[0x61] = info(Op::Cbeq, Mode::Sp1, 4, 6);
    t[0x6B] = info(Op::Dbnz, Mode::Sp1, 4, 6);

    for (size_t k = 0; k < 16; k++) {
        const Op op = kAluOps[k];
        if (op == Op::Jmp || op == Op::Jsr) {
            continue;
        }

        t[0xD0 + k] = info(op, Mode::Sp2, 4, 5);
        t[0xE0 + k] = info(op, Mode::Sp1, 3, 4);
    }

    return t;
}

}  // namespace isa_detail

inline constexpr std::array<OpInfo, 256> kOpcodes = isa_detail::make_base();
inline constexpr std::array<OpInfo, 256> kOpcodes9E = isa_detail::make_page9e();

// The instruction at `pc`: `fetch(addr)` reads a code byte.
template <class Fetch>
[[gnu::always_inline]] inline Insn decode(uint16_t pc, Fetch&& fetch) {
    const uint8_t opcode = fetch(pc);
    const bool prefixed = opcode == 0x9E;
    const uint8_t second = prefixed ? fetch(uint16_t(pc + 1)) : 0;
    const OpInfo& info = prefixed ? kOpcodes9E[second] : kOpcodes[opcode];

    Insn insn;
    insn.op = info.op;
    insn.mode = info.mode;
    insn.length = info.length;
    insn.cycles = info.cycles;

    if (info.op == Op::Illegal) {
        // the opcode for the report: $9Exx for the page, else the byte
        insn.b[0] = prefixed ? 0x9E : 0;
        insn.b[1] = prefixed ? second : opcode;
        insn.length = prefixed ? 2 : 1;
        return insn;
    }

    const uint16_t operands = uint16_t(pc + (prefixed ? 2 : 1));
    const int count = info.length - (prefixed ? 2 : 1);
    for (int k = 0; k < count; k++) {
        insn.b[k] = fetch(uint16_t(operands + k));
    }

    if (!prefixed && opcode < 0x20) {
        insn.bit = uint8_t((opcode & 0x0F) >> 1);
    }

    return insn;
}

// The mnemonic (lower case), for disassembly and traces.
const char* mnemonic(Op op);

}  // namespace sim
