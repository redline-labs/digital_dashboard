// The e200's instruction set: decode() turns an instruction word into an Insn (16 bytes) that every engine runs with
// the same execute_insn() (e200/cpu.h).
//
// Covered: the 32-bit Book E user instructions (integer, logical, rotate and shift, compare, branch, condition
// register, load and store including multiple, byte-reversed and reserved forms), the e200's isel and wait, the
// supervisor instructions (MSR, SPRs, rfi/rfci/rfdi/rfmci, the TLB), the cache and synchronisation instructions (no-ops
// but for dcbz), and the SPE's scalar single-precision instructions (efs*). Everything else -- floating point, the SPE
// vector instructions, VLE -- decodes as Illegal, which the core takes as a program interrupt as the part would.
//
// Field names follow the Book E encodings; the Insn keeps the raw register fields by position so one decoder serves
// every form:
//   d  bits 6-10  (rD, rS, BO, crbD, crfD << 2 for the compares, TO)
//   a  bits 11-15 (rA, BI, crbA)
//   b  bits 16-20 (rB, SH, NB, crbB)
//   c  bits 21-25 (MB, isel's crb)
//   e  bits 26-30 (ME)
#pragma once

#include <cstdint>
#include <string>

namespace sim::e200 {

enum class Op : uint8_t {
    Illegal,
    // integer arithmetic
    Addi, Addis, Addic, AddicRc, Subfic, Mulli,
    Add, Addc, Adde, Addme, Addze, Subf, Subfc, Subfe, Subfme, Subfze, Neg,
    Mullw, Mulhw, Mulhwu, Divw, Divwu,
    // logical
    AndiRc, AndisRc, Ori, Oris, Xori, Xoris,
    And, Andc, Or, Orc, Xor, Nand, Nor, Eqv, Extsb, Extsh, Cntlzw,
    // rotate and shift
    Rlwinm, Rlwnm, Rlwimi, Slw, Srw, Sraw, Srawi,
    // compare
    Cmp, Cmpi, Cmpl, Cmpli,
    // branch
    B, Bc, Bclr, Bcctr,
    // condition register
    Mcrf, Crand, Crandc, Creqv, Crnand, Crnor, Cror, Crorc, Crxor, Mfcr, Mtcrf, Mcrxr,
    Isel,
    // load and store
    Lbz, Lbzu, Lbzx, Lbzux, Lhz, Lhzu, Lhzx, Lhzux, Lha, Lhau, Lhax, Lhaux, Lwz, Lwzu, Lwzx, Lwzux,
    Stb, Stbu, Stbx, Stbux, Sth, Sthu, Sthx, Sthux, Stw, Stwu, Stwx, Stwux,
    Lhbrx, Lwbrx, Sthbrx, Stwbrx, Lmw, Stmw, Lwarx, StwcxRc,
    // system
    Sc, Tw, Twi, Mfspr, Mtspr, Mfmsr, Mtmsr, Wrtee, Wrteei, Rfi, Rfci, Rfdi, Rfmci, Wait,
    Isync, Msync, Mbar, CacheNop, Dcbz,
    Tlbwe, Tlbre, Tlbsx, Tlbivax, Tlbsync,
    // SPE scalar single precision
    Efsadd, Efssub, Efsmul, Efsdiv, Efsabs, Efsnabs, Efsneg,
    Efscmpgt, Efscmplt, Efscmpeq, Efststgt, Efststlt, Efststeq,
    Efscfsi, Efscfui, Efscfsf, Efscfuf, Efsctsi, Efsctui, Efsctsiz, Efsctuiz, Efsctsf, Efsctuf,
    Count_,
};

namespace flag {
inline constexpr uint8_t kRc = 0x01;  // record: CR0 from the result (the "." forms)
inline constexpr uint8_t kOe = 0x02;  // overflow enable: XER[OV,SO] from the result
inline constexpr uint8_t kLk = 0x04;  // link: LR = the next instruction
inline constexpr uint8_t kAa = 0x08;  // absolute branch target
}  // namespace flag

struct Insn {
    Op op = Op::Illegal;
    uint8_t d = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    uint8_t c = 0;
    uint8_t e = 0;
    uint8_t flags = 0;
    uint16_t spr = 0;  // mfspr/mtspr: the SPR number, its two 5-bit halves swapped back
    int32_t imm = 0;   // SIMM / UIMM / branch displacement (bytes) / mtcrf's CRM / wrteei's E / twi's SIMM
    uint32_t word = 0;

    bool has(uint8_t f) const {
        return (flags & f) != 0;
    }
};

Insn decode(uint32_t word);

// True for the instructions after which a block must end: control flow, anything that changes the MSR, the
// translation (the TLB, PID) or the timebase, and what may raise an exception by itself (sc, traps, illegal).
bool ends_block(const Insn& insn);

// The instruction's mnemonic ("addi", "stwcx.", "efsadd").
const char* mnemonic(const Insn& insn);

// "addi r3,r1,8", "bc 12,2,0x100" (branch targets as addresses, for `pc`).
std::string disassemble(const Insn& insn, uint32_t pc);

}  // namespace sim::e200
