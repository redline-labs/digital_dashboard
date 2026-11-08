// The host-neutral interface the translator drives (as mk60e5's BlockAssembler): six 32-bit scratch registers,
// labels, guest-state fields, guest memory with its fast and slow paths, and the step helper. One implementation per
// host, compiled only on its architecture: assembler_aarch64.cpp, assembler_x86_64.cpp.
//
// Registers hold 32-bit values; the translator masks to 8 and 16 bits itself. A helper call (the slow memory paths,
// call_step) preserves every scratch register except its destination.
//
// The guest registers A, HX, SP, CCR and the cycle count live in host registers: load_state/store_state on those
// fields are register moves, add_cycles is one add. They are written back to the CpuState by leave() and before every
// helper call, and reloaded after call_step (the interpreter changed them).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "hc08/isa.h"

namespace sim::jit {

enum class JOp : uint8_t { Add, Sub, And, Or, Xor, Shl, Shr, Mul };
enum class JCond : uint8_t { Eq, Ne, Ltu, Geu, Gtu, Leu };

inline constexpr int kScratchRegisters = 6;

class BlockAssembler {
   public:
    virtual ~BlockAssembler() = default;

    // saves the host's registers, loads the bases and the guest registers, and binds the body (a chained jump's target)
    virtual void prologue() = 0;
    // where the body starts, from the start of the code (after the code is flattened)
    virtual uint64_t body_offset() = 0;
    // writes the guest registers back and returns from the block
    virtual void leave() = 0;
    // the block's end with the next pc known (an immediate, or a register): chain into the next block when the engine
    // loop would run it now, else store the pc and leave
    virtual void dispatch_imm(uint16_t pc) = 0;
    virtual void dispatch_reg(int pc) = 0;

    virtual size_t new_label() = 0;
    virtual void bind(size_t label) = 0;
    virtual void jump(size_t label) = 0;

    // d = the 1- or 2-byte field at `offset` in the CpuState, zero-extended
    virtual void load_state(int d, int32_t offset, int bytes) = 0;
    virtual void store_state(int32_t offset, int s, int bytes) = 0;
    virtual void store_state_imm(int32_t offset, uint32_t value, int bytes) = 0;
    // CpuState::cycles += n
    virtual void add_cycles(uint32_t n) = 0;

    virtual void move_imm(int d, uint32_t value) = 0;
    virtual void move(int d, int s) = 0;
    virtual void op(JOp op, int d, int a, int b) = 0;
    virtual void op_imm(JOp op, int d, int a, uint32_t imm) = 0;
    // d = (a cond imm) ? 1 : 0
    virtual void set_if(JCond cond, int d, int a, uint32_t imm) = 0;
    virtual void branch_if(JCond cond, int a, uint32_t imm, size_t label) = 0;

    // d = guest byte at `addr` (16-bit value in a register); `pc` is stored to the state before a slow call
    virtual void read8(int d, int addr, uint16_t pc) = 0;
    virtual void write8(int addr, int value, uint16_t pc) = 0;
    // d = context exit flag
    virtual void load_exit(int d) = 0;
    // d = the environment's interrupt lines (non-zero: an interrupt is pending)
    virtual void load_irq_lines(int d) = 0;
    // d = step(insn, remaining)
    virtual void call_step(int d, const Insn* insn, uint32_t remaining) = 0;
};

// The host's assembler writing into `code` (an asmjit::CodeHolder), or null when the host has none.
std::unique_ptr<BlockAssembler> make_host_assembler(void* code_holder);

}  // namespace sim::jit
