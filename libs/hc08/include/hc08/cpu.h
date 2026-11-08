// The CPU08 core: the semantics of every instruction (execute_insn) and the run loop every engine shares (run_core).
//
// The environment is a template parameter, so memory accesses inline and the loop has no virtual calls. It must
// provide (duck-typed, as mk60e5's Env):
//
//   uint8_t  fetch(uint16_t addr)          a code byte (no side effects)
//   uint8_t  read(uint16_t addr)           a data read (device registers may have side effects)
//   void     write(uint16_t addr, uint8_t)
//   uint64_t cycle_limit() const           run until CpuState::cycles reaches it (re-read every instruction: the
//                                          machine lowers it when a device access moves an event earlier)
//   bool     irq_pending() const           an enabled interrupt source is asserted
//   uint16_t irq_vector() const            the vector address of the highest-priority one ($FFxx)
//   bool     watching() const              any pc watch is set (cheap test before pc_watched)
//   bool     pc_watched(uint16_t pc) const
//   bool     irq_pin() const               the IRQ pin level, for BIL/BIH
//
// Semantics follow the CPU08 Reference Manual (CPU08RM Rev 4, section 7). Interrupt entry stacks PCL, PCH, X, A, CCR
// (not H: the firmware's handlers push it themselves) and sets I; RTI unstacks in reverse.
#pragma once

#include <cstdint>

#include "hc08/cpu_state.h"
#include "hc08/isa.h"

namespace sim {

namespace cpu_detail {

[[gnu::always_inline]] inline void set_nz(CpuState& s, uint8_t value) {
    s.set_flag(ccr::kN, (value & 0x80) != 0);
    s.set_flag(ccr::kZ, value == 0);
}

[[gnu::always_inline]] inline void set_nz16(CpuState& s, uint16_t value) {
    s.set_flag(ccr::kN, (value & 0x8000) != 0);
    s.set_flag(ccr::kZ, value == 0);
}

// a + b + carry: H, V, N, Z, C
[[gnu::always_inline]] inline uint8_t add8(CpuState& s, uint8_t a, uint8_t b, bool carry) {
    const unsigned sum = unsigned(a) + b + (carry ? 1 : 0);
    const uint8_t r = uint8_t(sum);
    s.set_flag(ccr::kH, ((a & 0x0F) + (b & 0x0F) + (carry ? 1 : 0)) > 0x0F);
    s.set_flag(ccr::kV, ((a ^ r) & (b ^ r) & 0x80) != 0);
    s.set_flag(ccr::kC, sum > 0xFF);
    set_nz(s, r);
    return r;
}

// a - b - borrow: V, N, Z, C (H unaffected)
[[gnu::always_inline]] inline uint8_t sub8(CpuState& s, uint8_t a, uint8_t b, bool borrow) {
    const int diff = int(a) - int(b) - (borrow ? 1 : 0);
    const uint8_t r = uint8_t(diff);
    s.set_flag(ccr::kV, ((a ^ b) & (a ^ r) & 0x80) != 0);
    s.set_flag(ccr::kC, diff < 0);
    set_nz(s, r);
    return r;
}

[[gnu::always_inline]] inline void logic_flags(CpuState& s, uint8_t r) {
    s.set_flag(ccr::kV, false);
    set_nz(s, r);
}

// the shift/rotate V rule: V = N xor C (after the operation)
[[gnu::always_inline]] inline void shift_flags(CpuState& s, uint8_t r, bool carry) {
    s.set_flag(ccr::kC, carry);
    set_nz(s, r);
    s.set_flag(ccr::kV, s.flag(ccr::kN) != carry);
}

template <class Env>
[[gnu::always_inline]] inline void push(CpuState& s, Env& env, uint8_t value) {
    env.write(s.sp, value);
    s.sp = uint16_t(s.sp - 1);
}

template <class Env>
[[gnu::always_inline]] inline uint8_t pull(CpuState& s, Env& env) {
    s.sp = uint16_t(s.sp + 1);
    return env.read(s.sp);
}

// the effective address of a memory mode
[[gnu::always_inline]] inline uint16_t address(const CpuState& s, const Insn& in) {
    switch (in.mode) {
        case Mode::Dir:
        case Mode::DirDir:
        case Mode::DirIxP:
            return in.b[0];
        case Mode::Ext:
            return in.word();
        case Mode::Ix:
        case Mode::IxPlus:
            return s.hx;
        case Mode::Ix1:
        case Mode::Ix1Plus:
            return uint16_t(s.hx + in.b[0]);
        case Mode::Ix2:
            return uint16_t(s.hx + in.word());
        case Mode::Sp1:
            return uint16_t(s.sp + in.b[0]);
        case Mode::Sp2:
            return uint16_t(s.sp + in.word());
        case Mode::None:
        case Mode::Inh:
        case Mode::InhA:
        case Mode::InhX:
        case Mode::Imm:
        case Mode::Imm16:
        case Mode::Rel:
        case Mode::ImmDir:
            return 0;
    }
}

[[gnu::always_inline]] inline bool branch_taken(const CpuState& s, Op op, bool irq_pin) {
    const bool c = s.flag(ccr::kC);
    const bool z = s.flag(ccr::kZ);
    const bool n = s.flag(ccr::kN);
    const bool v = s.flag(ccr::kV);

    switch (op) {
        case Op::Bra:
        case Op::Bsr:
            return true;
        case Op::Brn:
            return false;
        case Op::Bhi:
            return !(c || z);
        case Op::Bls:
            return c || z;
        case Op::Bcc:
            return !c;
        case Op::Bcs:
            return c;
        case Op::Bne:
            return !z;
        case Op::Beq:
            return z;
        case Op::Bhcc:
            return !s.flag(ccr::kH);
        case Op::Bhcs:
            return s.flag(ccr::kH);
        case Op::Bpl:
            return !n;
        case Op::Bmi:
            return n;
        case Op::Bmc:
            return !s.flag(ccr::kI);
        case Op::Bms:
            return s.flag(ccr::kI);
        case Op::Bil:
            return !irq_pin;
        case Op::Bih:
            return irq_pin;
        case Op::Bge:
            return n == v;
        case Op::Blt:
            return n != v;
        case Op::Bgt:
            return !(z || (n != v));
        case Op::Ble:
            return z || (n != v);
        case Op::Illegal:
        case Op::Brset:
        case Op::Brclr:
        case Op::Bset:
        case Op::Bclr:
        case Op::Neg:
        case Op::Com:
        case Op::Lsr:
        case Op::Ror:
        case Op::Asr:
        case Op::Lsl:
        case Op::Rol:
        case Op::Dec:
        case Op::Inc:
        case Op::Tst:
        case Op::Clr:
        case Op::Cbeq:
        case Op::Dbnz:
        case Op::Mov:
        case Op::Ldhx:
        case Op::Sthx:
        case Op::Cphx:
        case Op::Aix:
        case Op::Ais:
        case Op::Sub:
        case Op::Cmp:
        case Op::Sbc:
        case Op::Cpx:
        case Op::And:
        case Op::Bit:
        case Op::Lda:
        case Op::Sta:
        case Op::Eor:
        case Op::Adc:
        case Op::Ora:
        case Op::Add:
        case Op::Jmp:
        case Op::Jsr:
        case Op::Ldx:
        case Op::Stx:
        case Op::Mul:
        case Op::Div:
        case Op::Nsa:
        case Op::Daa:
        case Op::Rti:
        case Op::Rts:
        case Op::Swi:
        case Op::Tap:
        case Op::Tpa:
        case Op::Pula:
        case Op::Psha:
        case Op::Pulx:
        case Op::Pshx:
        case Op::Pulh:
        case Op::Pshh:
        case Op::Clrh:
        case Op::Stop:
        case Op::Wait:
        case Op::Txs:
        case Op::Tsx:
        case Op::Tax:
        case Op::Txa:
        case Op::Clc:
        case Op::Sec:
        case Op::Cli:
        case Op::Sei:
        case Op::Rsp:
        case Op::Nop:
            return false;
    }
}

// The read-modify-write operations on one byte; sets the flags.
[[gnu::always_inline]] inline uint8_t rmw(CpuState& s, Op op, uint8_t m) {
    switch (op) {
        case Op::Neg: {
            const uint8_t r = uint8_t(0 - m);
            s.set_flag(ccr::kV, r == 0x80);
            s.set_flag(ccr::kC, r != 0);
            set_nz(s, r);
            return r;
        }
        case Op::Com: {
            const uint8_t r = uint8_t(~m);
            s.set_flag(ccr::kV, false);
            s.set_flag(ccr::kC, true);
            set_nz(s, r);
            return r;
        }
        case Op::Lsr: {
            const uint8_t r = uint8_t(m >> 1);
            shift_flags(s, r, (m & 1) != 0);
            return r;
        }
        case Op::Ror: {
            const uint8_t r = uint8_t((m >> 1) | (s.flag(ccr::kC) ? 0x80 : 0));
            shift_flags(s, r, (m & 1) != 0);
            return r;
        }
        case Op::Asr: {
            const uint8_t r = uint8_t((m >> 1) | (m & 0x80));
            shift_flags(s, r, (m & 1) != 0);
            return r;
        }
        case Op::Lsl: {
            const uint8_t r = uint8_t(m << 1);
            shift_flags(s, r, (m & 0x80) != 0);
            return r;
        }
        case Op::Rol: {
            const uint8_t r = uint8_t((m << 1) | (s.flag(ccr::kC) ? 1 : 0));
            shift_flags(s, r, (m & 0x80) != 0);
            return r;
        }
        case Op::Dec: {
            const uint8_t r = uint8_t(m - 1);
            s.set_flag(ccr::kV, m == 0x80);
            set_nz(s, r);
            return r;
        }
        case Op::Inc: {
            const uint8_t r = uint8_t(m + 1);
            s.set_flag(ccr::kV, m == 0x7F);
            set_nz(s, r);
            return r;
        }
        case Op::Tst:
            logic_flags(s, m);
            return m;
        case Op::Clr:
            s.set_flag(ccr::kV, false);
            s.set_flag(ccr::kN, false);
            s.set_flag(ccr::kZ, true);
            return 0;
        case Op::Illegal:
        case Op::Brset:
        case Op::Brclr:
        case Op::Bset:
        case Op::Bclr:
        case Op::Bra:
        case Op::Brn:
        case Op::Bhi:
        case Op::Bls:
        case Op::Bcc:
        case Op::Bcs:
        case Op::Bne:
        case Op::Beq:
        case Op::Bhcc:
        case Op::Bhcs:
        case Op::Bpl:
        case Op::Bmi:
        case Op::Bmc:
        case Op::Bms:
        case Op::Bil:
        case Op::Bih:
        case Op::Bge:
        case Op::Blt:
        case Op::Bgt:
        case Op::Ble:
        case Op::Bsr:
        case Op::Cbeq:
        case Op::Dbnz:
        case Op::Mov:
        case Op::Ldhx:
        case Op::Sthx:
        case Op::Cphx:
        case Op::Aix:
        case Op::Ais:
        case Op::Sub:
        case Op::Cmp:
        case Op::Sbc:
        case Op::Cpx:
        case Op::And:
        case Op::Bit:
        case Op::Lda:
        case Op::Sta:
        case Op::Eor:
        case Op::Adc:
        case Op::Ora:
        case Op::Add:
        case Op::Jmp:
        case Op::Jsr:
        case Op::Ldx:
        case Op::Stx:
        case Op::Mul:
        case Op::Div:
        case Op::Nsa:
        case Op::Daa:
        case Op::Rti:
        case Op::Rts:
        case Op::Swi:
        case Op::Tap:
        case Op::Tpa:
        case Op::Pula:
        case Op::Psha:
        case Op::Pulx:
        case Op::Pshx:
        case Op::Pulh:
        case Op::Pshh:
        case Op::Clrh:
        case Op::Stop:
        case Op::Wait:
        case Op::Txs:
        case Op::Tsx:
        case Op::Tax:
        case Op::Txa:
        case Op::Clc:
        case Op::Sec:
        case Op::Cli:
        case Op::Sei:
        case Op::Rsp:
        case Op::Nop:
            return m;
    }
}

}  // namespace cpu_detail

// Interrupt (or SWI) entry: stack PCL, PCH, X, A, CCR, set I, jump through the vector. 9 cycles.
template <class Env>
inline void enter_interrupt(CpuState& s, Env& env, uint16_t vector, uint16_t return_pc) {
    using namespace cpu_detail;
    push(s, env, uint8_t(return_pc));
    push(s, env, uint8_t(return_pc >> 8));
    push(s, env, s.x());
    push(s, env, s.a);
    push(s, env, uint8_t(s.ccr | ccr::kOnes));
    s.set_flag(ccr::kI, true);
    s.pc = uint16_t((uint16_t(env.read(vector)) << 8) | env.read(uint16_t(vector + 1)));
    s.cycles += 9;
}

// Executes one decoded instruction at s.pc. Returns StopKind::None, or Wait/StopMode/Illegal.
template <class Env>
inline Stop execute_insn(CpuState& s, Env& env, const Insn& in) {
    using namespace cpu_detail;

    const uint16_t pc = s.pc;
    const uint16_t next = uint16_t(pc + in.length);
    s.pc = next;
    s.cycles += in.cycles;

    switch (in.op) {
        case Op::Illegal:
            s.pc = pc;
            s.cycles -= in.cycles;
            return Stop{StopKind::Illegal, pc, uint16_t((uint16_t(in.b[0]) << 8) | in.b[1])};

        // ---------------------------------------------------------------- bit manipulation
        case Op::Brset:
        case Op::Brclr: {
            const uint8_t m = env.read(in.b[0]);
            const bool set = ((m >> in.bit) & 1) != 0;
            s.set_flag(ccr::kC, set);
            if (set == (in.op == Op::Brset)) {
                s.pc = uint16_t(next + sext8(in.b[1]));
            }
            break;
        }
        case Op::Bset:
            env.write(in.b[0], uint8_t(env.read(in.b[0]) | (1u << in.bit)));
            break;
        case Op::Bclr:
            env.write(in.b[0], uint8_t(env.read(in.b[0]) & ~(1u << in.bit)));
            break;

        // ---------------------------------------------------------------- branches
        case Op::Bsr:
            push(s, env, uint8_t(next));
            push(s, env, uint8_t(next >> 8));
            s.pc = uint16_t(next + sext8(in.b[0]));
            break;
        case Op::Bra:
        case Op::Brn:
        case Op::Bhi:
        case Op::Bls:
        case Op::Bcc:
        case Op::Bcs:
        case Op::Bne:
        case Op::Beq:
        case Op::Bhcc:
        case Op::Bhcs:
        case Op::Bpl:
        case Op::Bmi:
        case Op::Bmc:
        case Op::Bms:
        case Op::Bil:
        case Op::Bih:
        case Op::Bge:
        case Op::Blt:
        case Op::Bgt:
        case Op::Ble:
            if (branch_taken(s, in.op, env.irq_pin())) {
                s.pc = uint16_t(next + sext8(in.b[0]));
            }
            break;

        // ---------------------------------------------------------------- read-modify-write
        case Op::Neg:
        case Op::Com:
        case Op::Lsr:
        case Op::Ror:
        case Op::Asr:
        case Op::Lsl:
        case Op::Rol:
        case Op::Dec:
        case Op::Inc:
        case Op::Tst:
        case Op::Clr:
            if (in.mode == Mode::InhA) {
                s.a = rmw(s, in.op, s.a);
            } else if (in.mode == Mode::InhX) {
                s.set_x(rmw(s, in.op, s.x()));
            } else {
                const uint16_t ea = address(s, in);
                if (in.op == Op::Clr) {
                    env.write(ea, rmw(s, in.op, 0));
                } else if (in.op == Op::Tst) {
                    rmw(s, in.op, env.read(ea));
                } else {
                    env.write(ea, rmw(s, in.op, env.read(ea)));
                }
            }
            break;

        case Op::Cbeq: {
            // compare A (X for CBEQX) with the operand; branch if equal; no flags
            uint8_t operand;
            uint8_t rel;
            uint8_t reg = s.a;

            switch (in.mode) {
                case Mode::InhA:
                    operand = in.b[0];
                    rel = in.b[1];
                    break;
                case Mode::InhX:
                    operand = in.b[0];
                    rel = in.b[1];
                    reg = s.x();
                    break;
                case Mode::IxPlus:
                    operand = env.read(s.hx);
                    rel = in.b[0];
                    s.hx = uint16_t(s.hx + 1);
                    break;
                case Mode::Ix1Plus:
                    operand = env.read(uint16_t(s.hx + in.b[0]));
                    rel = in.b[1];
                    s.hx = uint16_t(s.hx + 1);
                    break;
                case Mode::None:
                case Mode::Inh:
                case Mode::Imm:
                case Mode::Imm16:
                case Mode::Dir:
                case Mode::Ext:
                case Mode::Ix:
                case Mode::Ix1:
                case Mode::Ix2:
                case Mode::Sp1:
                case Mode::Sp2:
                case Mode::Rel:
                case Mode::DirDir:
                case Mode::DirIxP:
                case Mode::ImmDir:  // Dir, Sp1
                    operand = env.read(address(s, in));
                    rel = in.b[1];
                    break;
            }

            if (reg == operand) {
                s.pc = uint16_t(next + sext8(rel));
            }
            break;
        }

        case Op::Dbnz: {
            uint8_t value;
            uint8_t rel;

            if (in.mode == Mode::InhA) {
                value = s.a = uint8_t(s.a - 1);
                rel = in.b[0];
            } else if (in.mode == Mode::InhX) {
                value = uint8_t(s.x() - 1);
                s.set_x(value);
                rel = in.b[0];
            } else {
                const uint16_t ea = address(s, in);
                value = uint8_t(env.read(ea) - 1);
                env.write(ea, value);
                rel = in.mode == Mode::Ix ? in.b[0] : in.b[1];
            }

            if (value != 0) {
                s.pc = uint16_t(next + sext8(rel));
            }
            break;
        }

        case Op::Mov: {
            uint8_t value;
            uint16_t dest;

            switch (in.mode) {
                case Mode::DirDir:
                    value = env.read(in.b[0]);
                    dest = in.b[1];
                    break;
                case Mode::ImmDir:
                    value = in.b[0];
                    dest = in.b[1];
                    break;
                case Mode::DirIxP:
                    value = env.read(in.b[0]);
                    dest = s.hx;
                    s.hx = uint16_t(s.hx + 1);
                    break;
                case Mode::None:
                case Mode::Inh:
                case Mode::InhA:
                case Mode::InhX:
                case Mode::Imm:
                case Mode::Imm16:
                case Mode::Dir:
                case Mode::Ext:
                case Mode::Ix:
                case Mode::Ix1:
                case Mode::Ix2:
                case Mode::Sp1:
                case Mode::Sp2:
                case Mode::Rel:
                case Mode::IxPlus:
                case Mode::Ix1Plus:  // IxPlus: MOV ,X+,opr8
                    value = env.read(s.hx);
                    dest = in.b[0];
                    s.hx = uint16_t(s.hx + 1);
                    break;
            }

            env.write(dest, value);
            logic_flags(s, value);
            break;
        }

        // ---------------------------------------------------------------- 16-bit index register
        case Op::Ldhx: {
            if (in.mode == Mode::Imm16) {
                s.hx = in.word();
            } else {
                const uint16_t ea = in.b[0];
                s.hx = uint16_t((uint16_t(env.read(ea)) << 8) | env.read(uint16_t(ea + 1)));
            }

            s.set_flag(ccr::kV, false);
            set_nz16(s, s.hx);
            break;
        }
        case Op::Sthx: {
            env.write(in.b[0], s.h());
            env.write(uint16_t(in.b[0] + 1), s.x());
            s.set_flag(ccr::kV, false);
            set_nz16(s, s.hx);
            break;
        }
        case Op::Cphx: {
            uint16_t m;
            if (in.mode == Mode::Imm16) {
                m = in.word();
            } else {
                m = uint16_t((uint16_t(env.read(in.b[0])) << 8) | env.read(uint16_t(in.b[0] + 1)));
            }

            const uint16_t r = uint16_t(s.hx - m);
            s.set_flag(ccr::kV, ((s.hx ^ m) & (s.hx ^ r) & 0x8000) != 0);
            s.set_flag(ccr::kC, m > s.hx);
            set_nz16(s, r);
            break;
        }
        case Op::Aix:
            s.hx = uint16_t(s.hx + sext8(in.b[0]));
            break;
        case Op::Ais:
            s.sp = uint16_t(s.sp + sext8(in.b[0]));
            break;

        // ---------------------------------------------------------------- register / memory
        case Op::Sub:
        case Op::Cmp:
        case Op::Sbc:
        case Op::Cpx:
        case Op::And:
        case Op::Bit:
        case Op::Lda:
        case Op::Eor:
        case Op::Adc:
        case Op::Ora:
        case Op::Add:
        case Op::Ldx: {
            const uint8_t m = in.mode == Mode::Imm ? in.b[0] : env.read(address(s, in));

            switch (in.op) {
                case Op::Sub:
                    s.a = sub8(s, s.a, m, false);
                    break;
                case Op::Cmp:
                    sub8(s, s.a, m, false);
                    break;
                case Op::Sbc:
                    s.a = sub8(s, s.a, m, s.flag(ccr::kC));
                    break;
                case Op::Cpx:
                    sub8(s, s.x(), m, false);
                    break;
                case Op::And:
                    s.a = uint8_t(s.a & m);
                    logic_flags(s, s.a);
                    break;
                case Op::Bit:
                    logic_flags(s, uint8_t(s.a & m));
                    break;
                case Op::Lda:
                    s.a = m;
                    logic_flags(s, m);
                    break;
                case Op::Eor:
                    s.a = uint8_t(s.a ^ m);
                    logic_flags(s, s.a);
                    break;
                case Op::Adc:
                    s.a = add8(s, s.a, m, s.flag(ccr::kC));
                    break;
                case Op::Ora:
                    s.a = uint8_t(s.a | m);
                    logic_flags(s, s.a);
                    break;
                case Op::Add:
                    s.a = add8(s, s.a, m, false);
                    break;
                case Op::Illegal:
                case Op::Brset:
                case Op::Brclr:
                case Op::Bset:
                case Op::Bclr:
                case Op::Bra:
                case Op::Brn:
                case Op::Bhi:
                case Op::Bls:
                case Op::Bcc:
                case Op::Bcs:
                case Op::Bne:
                case Op::Beq:
                case Op::Bhcc:
                case Op::Bhcs:
                case Op::Bpl:
                case Op::Bmi:
                case Op::Bmc:
                case Op::Bms:
                case Op::Bil:
                case Op::Bih:
                case Op::Bge:
                case Op::Blt:
                case Op::Bgt:
                case Op::Ble:
                case Op::Bsr:
                case Op::Neg:
                case Op::Com:
                case Op::Lsr:
                case Op::Ror:
                case Op::Asr:
                case Op::Lsl:
                case Op::Rol:
                case Op::Dec:
                case Op::Inc:
                case Op::Tst:
                case Op::Clr:
                case Op::Cbeq:
                case Op::Dbnz:
                case Op::Mov:
                case Op::Ldhx:
                case Op::Sthx:
                case Op::Cphx:
                case Op::Aix:
                case Op::Ais:
                case Op::Sta:
                case Op::Jmp:
                case Op::Jsr:
                case Op::Ldx:
                case Op::Stx:
                case Op::Mul:
                case Op::Div:
                case Op::Nsa:
                case Op::Daa:
                case Op::Rti:
                case Op::Rts:
                case Op::Swi:
                case Op::Tap:
                case Op::Tpa:
                case Op::Pula:
                case Op::Psha:
                case Op::Pulx:
                case Op::Pshx:
                case Op::Pulh:
                case Op::Pshh:
                case Op::Clrh:
                case Op::Stop:
                case Op::Wait:
                case Op::Txs:
                case Op::Tsx:
                case Op::Tax:
                case Op::Txa:
                case Op::Clc:
                case Op::Sec:
                case Op::Cli:
                case Op::Sei:
                case Op::Rsp:
                case Op::Nop:  // Ldx
                    s.set_x(m);
                    logic_flags(s, m);
                    break;
            }
            break;
        }
        case Op::Sta:
            env.write(address(s, in), s.a);
            logic_flags(s, s.a);
            break;
        case Op::Stx:
            env.write(address(s, in), s.x());
            logic_flags(s, s.x());
            break;
        case Op::Jmp:
            s.pc = address(s, in);
            break;
        case Op::Jsr: {
            const uint16_t target = address(s, in);
            push(s, env, uint8_t(next));
            push(s, env, uint8_t(next >> 8));
            s.pc = target;
            break;
        }

        // ---------------------------------------------------------------- inherent
        case Op::Mul: {
            const uint16_t product = uint16_t(uint16_t(s.x()) * s.a);
            s.a = uint8_t(product);
            s.set_x(uint8_t(product >> 8));
            s.set_flag(ccr::kH, false);
            s.set_flag(ccr::kC, false);
            break;
        }
        case Op::Div: {
            // A = H:A / X, H = remainder; C on divide by zero or a quotient over $FF (A and H then undefined: kept)
            const uint16_t dividend = uint16_t((uint16_t(s.h()) << 8) | s.a);
            const uint8_t divisor = s.x();
            if (divisor == 0 || dividend / divisor > 0xFF) {
                s.set_flag(ccr::kC, true);
                s.set_flag(ccr::kZ, false);
            } else {
                s.a = uint8_t(dividend / divisor);
                s.set_h(uint8_t(dividend % divisor));
                s.set_flag(ccr::kC, false);
                s.set_flag(ccr::kZ, s.a == 0);
            }
            break;
        }
        case Op::Nsa:
            s.a = uint8_t((s.a << 4) | (s.a >> 4));
            break;
        case Op::Daa: {
            const uint8_t a = s.a;
            const bool c = s.flag(ccr::kC);
            const bool h = s.flag(ccr::kH);
            uint8_t correction = 0;
            bool carry_out = c;

            if (h || (a & 0x0F) > 9) {
                correction |= 0x06;
            }
            if (c || a > 0x99) {
                correction |= 0x60;
                carry_out = true;
            }

            s.a = uint8_t(a + correction);
            s.set_flag(ccr::kC, carry_out);
            s.set_flag(ccr::kV, ((a ^ s.a) & (correction ^ s.a) & 0x80) != 0);
            set_nz(s, s.a);
            break;
        }
        case Op::Rti: {
            s.ccr = uint8_t(pull(s, env) | ccr::kOnes);
            s.a = pull(s, env);
            s.set_x(pull(s, env));
            const uint8_t hi = pull(s, env);
            s.pc = uint16_t((uint16_t(hi) << 8) | pull(s, env));
            break;
        }
        case Op::Rts: {
            const uint8_t hi = pull(s, env);
            s.pc = uint16_t((uint16_t(hi) << 8) | pull(s, env));
            break;
        }
        case Op::Swi:
            s.cycles -= in.cycles;  // enter_interrupt counts the 9
            enter_interrupt(s, env, 0xFFFC, next);
            break;
        case Op::Tap: {
            const bool was_masked = s.flag(ccr::kI);
            s.ccr = uint8_t(s.a | ccr::kOnes);
            if (was_masked && !s.flag(ccr::kI)) {
                s.irq_inhibit = true;
            }
            break;
        }
        case Op::Tpa:
            s.a = uint8_t(s.ccr | ccr::kOnes);
            break;
        case Op::Pula:
            s.a = pull(s, env);
            break;
        case Op::Psha:
            push(s, env, s.a);
            break;
        case Op::Pulx:
            s.set_x(pull(s, env));
            break;
        case Op::Pshx:
            push(s, env, s.x());
            break;
        case Op::Pulh:
            s.set_h(pull(s, env));
            break;
        case Op::Pshh:
            push(s, env, s.h());
            break;
        case Op::Clrh:
            s.set_h(0);
            break;
        case Op::Stop:
            s.set_flag(ccr::kI, false);
            return Stop{StopKind::StopMode, s.pc, 0};
        case Op::Wait:
            s.set_flag(ccr::kI, false);
            return Stop{StopKind::Wait, s.pc, 0};
        case Op::Txs:
            s.sp = uint16_t(s.hx - 1);
            break;
        case Op::Tsx:
            s.hx = uint16_t(s.sp + 1);
            break;
        case Op::Tax:
            s.set_x(s.a);
            break;
        case Op::Txa:
            s.a = s.x();
            break;
        case Op::Clc:
            s.set_flag(ccr::kC, false);
            break;
        case Op::Sec:
            s.set_flag(ccr::kC, true);
            break;
        case Op::Cli:
            if (s.flag(ccr::kI)) {
                s.irq_inhibit = true;
            }
            s.set_flag(ccr::kI, false);
            break;
        case Op::Sei:
            s.set_flag(ccr::kI, true);
            break;
        case Op::Rsp:
            s.sp = uint16_t((s.sp & 0xFF00) | 0x00FF);
            break;
        case Op::Nop:
            break;
    }

    return Stop{};
}

// The run loop of every engine: interrupts, pc watches and the cycle limit are checked before each instruction, in
// the same order whatever supplies the decoded instruction (`next_insn(pc)`). Identical steps and stops across engines
// are the engines' contract (hc08/engines.h).
template <class Env, class NextInsn>
inline Stop run_core(CpuState& s, Env& env, NextInsn&& next_insn) {
    for (;;) {
        if (s.cycles >= env.cycle_limit()) {
            return Stop{StopKind::Limit, s.pc, 0};
        }

        const bool inhibit = s.irq_inhibit;
        s.irq_inhibit = false;

        if (!inhibit && !s.flag(ccr::kI) && env.irq_pending()) {
            enter_interrupt(s, env, env.irq_vector(), s.pc);
            continue;
        }

        if (env.watching() && int32_t(s.pc) != s.skip_watch_pc && env.pc_watched(s.pc)) {
            return Stop{StopKind::Watch, s.pc, 0};
        }
        s.skip_watch_pc = -1;

        const Stop stop = execute_insn(s, env, next_insn(s.pc));
        if (stop.kind != StopKind::None) {
            return stop;
        }
    }
}

// The interpreter: decodes every instruction as it runs it.
template <class Env>
inline Stop interpret(CpuState& s, Env& env) {
    return run_core(s, env,
                    [&env](uint16_t pc) { return decode(pc, [&env](uint16_t addr) { return env.fetch(addr); }); });
}

}  // namespace sim
