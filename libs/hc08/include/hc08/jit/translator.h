// The translator: a run of decoded CPU08 instructions (a block of the decode cache) to host code, through the
// host-neutral BlockAssembler.
//
// Each instruction gets a native form (loads, stores, ALU, read-modify-write, branches, the stack, the 16-bit index
// operations, MOV, CBEQ, DBNZ, BRSET/BRCLR/BSET/BCLR, MUL) or a call to the step helper, which runs it through the
// interpreter's execute_insn (DIV, DAA, NSA, RTI, SWI, TAP, TPA, CLI, SEI, RSP, STOP, WAIT, illegal opcodes). The
// semantics of the native forms are the interpreter's, flag for flag; hc08_engine_diff --engine-diff --engine jit holds them
// to it.
//
// A block ends at the first control-flow instruction, at a helper that can end it (anything but the plain ones), at
// 32 instructions, or before an instruction that starts in the next 256-byte page.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "hc08/jit/code_memory.h"
#include "hc08/jit/native_abi.h"
#include "hc08/isa.h"

namespace sim::jit {

inline constexpr int kMaxBlockInstructions = 32;

// True if the instruction ends a block (control flow, or a helper whose effect the engine must see).
bool ends_block(const Insn& insn);

struct TranslatedBlock {
    ChainEntry chain;  // what a chained exit into this block reads (JitCache's pc table points here)
    BlockEntry entry = nullptr;
    uint16_t pc = 0;
    uint32_t cycles = 0;  // the sum of the instructions' cycles (a block runs only when they fit the limit)
    std::vector<Insn> insns;
    uint32_t native = 0;  // instructions with a native form
    uint32_t helpers = 0;
};

// Translates `insns` (consecutive, starting at `pc`) into `memory`. Returns null when the code memory is full.
// `block->insns` must stay where it is while the code can run (the helper calls point into it).
std::unique_ptr<TranslatedBlock> translate(CodeMemory& memory, uint16_t pc, std::vector<Insn> insns);

// Whether this host has an assembler.
bool host_supported();

}  // namespace sim::jit
