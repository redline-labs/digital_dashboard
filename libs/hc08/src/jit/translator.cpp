#include "hc08/jit/translator.h"

#include <asmjit/core.h>

#include <cstring>
#include <utility>

#include "hc08/jit/block_assembler.h"
#include "hc08/jit/jit_error.h"

namespace sim::jit {

namespace {

// CCR bits (hc08/cpu_state.h)
constexpr uint32_t kC = ccr::kC;
constexpr uint32_t kZ = ccr::kZ;
constexpr uint32_t kN = ccr::kN;
constexpr uint32_t kI = ccr::kI;
constexpr uint32_t kH = ccr::kH;
constexpr uint32_t kV = ccr::kV;

// scratch register names
constexpr int R0 = 0;
constexpr int R1 = 1;
constexpr int R2 = 2;
constexpr int R3 = 3;
constexpr int R4 = 4;
constexpr int R5 = 5;

bool is_helper(Op op) {
    switch (op) {
        case Op::Div:
        case Op::Daa:
        case Op::Nsa:
        case Op::Rti:
        case Op::Swi:
        case Op::Tap:
        case Op::Tpa:
        case Op::Rsp:
        case Op::Stop:
        case Op::Wait:
        case Op::Illegal:
            return true;
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
        case Op::Rts:
        case Op::Pula:
        case Op::Psha:
        case Op::Pulx:
        case Op::Pshx:
        case Op::Pulh:
        case Op::Pshh:
        case Op::Clrh:
        case Op::Txs:
        case Op::Tsx:
        case Op::Tax:
        case Op::Txa:
        case Op::Clc:
        case Op::Sec:
        case Op::Cli:
        case Op::Sei:
        case Op::Nop:
            return false;
    }
}

class Translator {
   public:
    Translator(BlockAssembler& a, uint16_t pc) : a_(a), pc_(pc) {
    }

    // one instruction at pc_; `remaining` = the cycles of the instructions after it; returns false when it ended the
    // block (the code has left)
    bool instruction(const Insn& in, uint32_t remaining, bool last);

   private:
    uint16_t next() const {
        return uint16_t(pc_ + insn_->length);
    }

    // the block's end, the next pc known: chain into the next block or leave (BlockAssembler::dispatch_*)
    void leave_at(uint16_t pc) {
        a_.dispatch_imm(pc);
    }
    void leave_at_reg(int target) {
        a_.dispatch_reg(target);
    }

    // leave for the engine loop at once (a helper access changed what it must check)
    void exit_now(uint16_t pc) {
        a_.store_state_imm(kStatePc, pc, 2);
        a_.leave();
    }

    // d = the effective address of a memory mode
    void address(int d) {
        const Insn& in = *insn_;
        switch (in.mode) {
            case Mode::Dir:
            case Mode::DirDir:
            case Mode::DirIxP:
                a_.move_imm(d, in.b[0]);
                break;
            case Mode::Ext:
                a_.move_imm(d, in.word());
                break;
            case Mode::Ix:
            case Mode::IxPlus:
                a_.load_state(d, kStateHx, 2);
                break;
            case Mode::Ix1:
            case Mode::Ix1Plus:
                a_.load_state(d, kStateHx, 2);
                a_.op_imm(JOp::Add, d, d, in.b[0]);
                a_.op_imm(JOp::And, d, d, 0xFFFF);
                break;
            case Mode::Ix2:
                a_.load_state(d, kStateHx, 2);
                a_.op_imm(JOp::Add, d, d, in.word());
                a_.op_imm(JOp::And, d, d, 0xFFFF);
                break;
            case Mode::Sp1:
                a_.load_state(d, kStateSp, 2);
                a_.op_imm(JOp::Add, d, d, in.b[0]);
                a_.op_imm(JOp::And, d, d, 0xFFFF);
                break;
            case Mode::Sp2:
                a_.load_state(d, kStateSp, 2);
                a_.op_imm(JOp::Add, d, d, in.word());
                a_.op_imm(JOp::And, d, d, 0xFFFF);
                break;
            case Mode::None:
            case Mode::Inh:
            case Mode::InhA:
            case Mode::InhX:
            case Mode::Imm:
            case Mode::Imm16:
            case Mode::Rel:
            case Mode::ImmDir:
                a_.move_imm(d, 0);
                break;
        }
    }

    void read(int d, int addr) {
        a_.read8(d, addr, next());
    }
    void write(int addr, int value) {
        a_.write8(addr, value, next());
    }

    // ccr = (ccr & ~mask) | bits (bits already within mask); uses R5
    void update_ccr(uint32_t mask, int bits) {
        a_.load_state(R5, kStateCcr, 1);
        a_.op_imm(JOp::And, R5, R5, ~mask & 0xFF);
        a_.op(JOp::Or, R5, R5, bits);
        a_.store_state(kStateCcr, R5, 1);
    }

    // bits |= N and Z of the 8-bit value r; uses t
    void add_nz(int bits, int r, int t) {
        a_.op_imm(JOp::Shr, t, r, 5);
        a_.op_imm(JOp::And, t, t, kN);
        a_.op(JOp::Or, bits, bits, t);
        a_.set_if(JCond::Eq, t, r, 0);
        a_.op_imm(JOp::Shl, t, t, 1);
        a_.op(JOp::Or, bits, bits, t);
    }

    // V=0, N, Z of r (LDA, STA, AND, ...): uses R3, R4
    void logic_flags(int r) {
        a_.move_imm(R3, 0);
        add_nz(R3, r, R4);
        update_ccr(kV | kN | kZ, R3);
    }

    // the shift/rotate flags: C from `carry` (0/1), N, Z, V = N ^ C; uses R3, R4
    void shift_flags(int r, int carry) {
        a_.move(R3, carry);
        add_nz(R3, r, R4);
        // V = N ^ C, at bit 7: (bits >> 2) ^ bits, bit 0 -> 7
        a_.op_imm(JOp::Shr, R4, R3, 2);
        a_.op(JOp::Xor, R4, R4, R3);
        a_.op_imm(JOp::And, R4, R4, 1);
        a_.op_imm(JOp::Shl, R4, R4, 7);
        a_.op(JOp::Or, R3, R3, R4);
        update_ccr(kV | kN | kZ | kC, R3);
    }

    // d = the carry flag (0/1)
    void carry_in(int d) {
        a_.load_state(d, kStateCcr, 1);
        a_.op_imm(JOp::And, d, d, kC);
    }

    void push(int value) {
        a_.load_state(R4, kStateSp, 2);
        write(R4, value);
        a_.op_imm(JOp::Sub, R4, R4, 1);
        a_.op_imm(JOp::And, R4, R4, 0xFFFF);
        a_.store_state(kStateSp, R4, 2);
    }

    void pull(int d) {
        a_.load_state(R4, kStateSp, 2);
        a_.op_imm(JOp::Add, R4, R4, 1);
        a_.op_imm(JOp::And, R4, R4, 0xFFFF);
        a_.store_state(kStateSp, R4, 2);
        read(d, R4);
    }

    void push_return(uint16_t ret) {
        a_.move_imm(R1, ret & 0xFF);
        push(R1);
        a_.move_imm(R1, ret >> 8);
        push(R1);
    }

    // X's byte of HX
    void load_x(int d) {
        a_.load_state(d, kStateHx, 2);
        a_.op_imm(JOp::And, d, d, 0xFF);
    }
    void store_x(int v) {
        a_.load_state(R5, kStateHx, 2);
        a_.op_imm(JOp::And, R5, R5, 0xFF00);
        a_.op(JOp::Or, R5, R5, v);
        a_.store_state(kStateHx, R5, 2);
    }

    // HX += 1 (the ,X+ forms)
    void increment_hx() {
        a_.load_state(R5, kStateHx, 2);
        a_.op_imm(JOp::Add, R5, R5, 1);
        a_.op_imm(JOp::And, R5, R5, 0xFFFF);
        a_.store_state(kStateHx, R5, 2);
    }

    // conditional end: taken -> target, else -> next; `taken` is a register 0/1
    void branch_on(int taken, uint16_t target) {
        const size_t yes = a_.new_label();
        a_.branch_if(JCond::Ne, taken, 0, yes);
        leave_at(next());
        a_.bind(yes);
        leave_at(target);
    }

    uint16_t rel_target(uint8_t rel) const {
        return uint16_t(next() + sext8(rel));
    }

    void branch(const Insn& in);
    void rmw(const Insn& in);
    void alu(const Insn& in);
    void helper(const Insn& in, uint32_t remaining);

    BlockAssembler& a_;
    uint16_t pc_;
    const Insn* insn_ = nullptr;
    // the previous instruction was a native CLI: after this one, leave if an interrupt can now be taken
    bool check_irq_after_ = false;
};

// the boolean of a branch condition into R1 (from the CCR in R0)
void Translator::branch(const Insn& in) {
    a_.load_state(R0, kStateCcr, 1);
    const auto bit = [&](uint32_t mask, bool set) {
        a_.op_imm(JOp::And, R1, R0, mask);
        a_.set_if(set ? JCond::Ne : JCond::Eq, R1, R1, 0);
    };
    // R2 = N ^ V (0/1)
    const auto n_xor_v = [&]() {
        a_.op_imm(JOp::Shr, R2, R0, 2);
        a_.op_imm(JOp::Shr, R3, R0, 7);
        a_.op(JOp::Xor, R2, R2, R3);
        a_.op_imm(JOp::And, R2, R2, 1);
    };

    switch (in.op) {
        case Op::Bra:
            leave_at(rel_target(in.b[0]));
            return;
        case Op::Brn:
            leave_at(next());
            return;
        case Op::Bil:  // the IRQ pin reads high (Env::irq_pin on every environment here)
            leave_at(next());
            return;
        case Op::Bih:
            leave_at(rel_target(in.b[0]));
            return;
        case Op::Bhi:
            bit(kC | kZ, false);
            break;
        case Op::Bls:
            bit(kC | kZ, true);
            break;
        case Op::Bcc:
            bit(kC, false);
            break;
        case Op::Bcs:
            bit(kC, true);
            break;
        case Op::Bne:
            bit(kZ, false);
            break;
        case Op::Beq:
            bit(kZ, true);
            break;
        case Op::Bhcc:
            bit(kH, false);
            break;
        case Op::Bhcs:
            bit(kH, true);
            break;
        case Op::Bpl:
            bit(kN, false);
            break;
        case Op::Bmi:
            bit(kN, true);
            break;
        case Op::Bmc:
            bit(kI, false);
            break;
        case Op::Bms:
            bit(kI, true);
            break;
        case Op::Bge:
            n_xor_v();
            a_.set_if(JCond::Eq, R1, R2, 0);
            break;
        case Op::Blt:
            n_xor_v();
            a_.move(R1, R2);
            break;
        case Op::Bgt:
        case Op::Ble:
            n_xor_v();
            a_.op_imm(JOp::Shr, R3, R0, 1);
            a_.op_imm(JOp::And, R3, R3, 1);
            a_.op(JOp::Or, R2, R2, R3);  // Z | (N ^ V)
            a_.set_if(in.op == Op::Bgt ? JCond::Eq : JCond::Ne, R1, R2, 0);
            break;
        case Op::Illegal:
        case Op::Brset:
        case Op::Brclr:
        case Op::Bset:
        case Op::Bclr:
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
            break;
    }
    branch_on(R1, rel_target(in.b[0]));
}

// read-modify-write on A, X or memory: the value in R0, the address (memory) in R2
void Translator::rmw(const Insn& in) {
    const bool on_a = in.mode == Mode::InhA;
    const bool on_x = in.mode == Mode::InhX;
    const bool memory = !on_a && !on_x;

    if (memory) {
        address(R2);
        if (in.op != Op::Clr) {
            read(R0, R2);
        }
    } else if (on_a) {
        a_.load_state(R0, kStateA, 1);
    } else {
        load_x(R0);
    }

    // R1 = result
    switch (in.op) {
        case Op::Neg:
            a_.move_imm(R1, 0);
            a_.op(JOp::Sub, R1, R1, R0);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            a_.set_if(JCond::Eq, R3, R1, 0x80);  // V
            a_.op_imm(JOp::Shl, R3, R3, 7);
            a_.set_if(JCond::Ne, R4, R1, 0);  // C
            a_.op(JOp::Or, R3, R3, R4);
            add_nz(R3, R1, R4);
            update_ccr(kV | kN | kZ | kC, R3);
            break;
        case Op::Com:
            a_.op_imm(JOp::Xor, R1, R0, 0xFF);
            a_.move_imm(R3, kC);
            add_nz(R3, R1, R4);
            update_ccr(kV | kN | kZ | kC, R3);
            break;
        case Op::Lsr:
            a_.op_imm(JOp::And, R4, R0, 1);
            a_.move(R5, R4);
            a_.op_imm(JOp::Shr, R1, R0, 1);
            shift_flags(R1, R5);
            break;
        case Op::Asr:
            a_.op_imm(JOp::And, R5, R0, 1);
            a_.op_imm(JOp::Shr, R1, R0, 1);
            a_.op_imm(JOp::And, R4, R0, 0x80);
            a_.op(JOp::Or, R1, R1, R4);
            shift_flags(R1, R5);
            break;
        case Op::Lsl:
            a_.op_imm(JOp::Shr, R5, R0, 7);
            a_.op_imm(JOp::Shl, R1, R0, 1);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            shift_flags(R1, R5);
            break;
        case Op::Rol:
            carry_in(R4);
            a_.op_imm(JOp::Shr, R5, R0, 7);
            a_.op_imm(JOp::Shl, R1, R0, 1);
            a_.op(JOp::Or, R1, R1, R4);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            shift_flags(R1, R5);
            break;
        case Op::Ror:
            carry_in(R4);
            a_.op_imm(JOp::Shl, R4, R4, 7);
            a_.op_imm(JOp::And, R5, R0, 1);
            a_.op_imm(JOp::Shr, R1, R0, 1);
            a_.op(JOp::Or, R1, R1, R4);
            shift_flags(R1, R5);
            break;
        case Op::Dec:
            a_.op_imm(JOp::Sub, R1, R0, 1);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            a_.set_if(JCond::Eq, R3, R0, 0x80);
            a_.op_imm(JOp::Shl, R3, R3, 7);
            add_nz(R3, R1, R4);
            update_ccr(kV | kN | kZ, R3);
            break;
        case Op::Inc:
            a_.op_imm(JOp::Add, R1, R0, 1);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            a_.set_if(JCond::Eq, R3, R0, 0x7F);
            a_.op_imm(JOp::Shl, R3, R3, 7);
            add_nz(R3, R1, R4);
            update_ccr(kV | kN | kZ, R3);
            break;
        case Op::Tst:
            a_.move(R1, R0);
            logic_flags(R1);
            return;  // no write back
        case Op::Clr:
            a_.move_imm(R1, 0);
            a_.move_imm(R3, kZ);
            update_ccr(kV | kN | kZ, R3);
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
            break;
    }

    if (memory) {
        write(R2, R1);
    } else if (on_a) {
        a_.store_state(kStateA, R1, 1);
    } else {
        store_x(R1);
    }
}

// the register/memory row: operand in R1, register in R0, result in R2
void Translator::alu(const Insn& in) {
    if (in.op == Op::Sta || in.op == Op::Stx) {
        address(R2);
        if (in.op == Op::Sta) {
            a_.load_state(R0, kStateA, 1);
        } else {
            load_x(R0);
        }
        write(R2, R0);
        logic_flags(R0);
        return;
    }

    if (in.mode == Mode::Imm) {
        a_.move_imm(R1, in.b[0]);
    } else {
        address(R2);
        read(R1, R2);
    }
    if (in.op == Op::Cpx) {
        load_x(R0);
    } else {
        a_.load_state(R0, kStateA, 1);
    }

    switch (in.op) {
        case Op::Lda:
            a_.store_state(kStateA, R1, 1);
            logic_flags(R1);
            break;
        case Op::Ldx:
            store_x(R1);
            logic_flags(R1);
            break;
        case Op::And:
        case Op::Ora:
        case Op::Eor:
        case Op::Bit:
            a_.op(in.op == Op::Ora ? JOp::Or : in.op == Op::Eor ? JOp::Xor : JOp::And, R2, R0, R1);
            if (in.op != Op::Bit) {
                a_.store_state(kStateA, R2, 1);
            }
            logic_flags(R2);
            break;
        case Op::Add:
        case Op::Adc: {
            if (in.op == Op::Adc) {
                carry_in(R4);
            } else {
                a_.move_imm(R4, 0);
            }
            a_.op(JOp::Add, R2, R0, R1);
            a_.op(JOp::Add, R2, R2, R4);  // the 9-bit sum
            // H: ((a & F) + (m & F) + c) & $10
            a_.op_imm(JOp::And, R3, R0, 0x0F);
            a_.op_imm(JOp::And, R5, R1, 0x0F);
            a_.op(JOp::Add, R3, R3, R5);
            a_.op(JOp::Add, R3, R3, R4);
            a_.op_imm(JOp::And, R3, R3, kH);
            // C: bit 8
            a_.op_imm(JOp::Shr, R4, R2, 8);
            a_.op_imm(JOp::And, R4, R4, 1);
            a_.op(JOp::Or, R3, R3, R4);
            a_.op_imm(JOp::And, R2, R2, 0xFF);
            // V: (a ^ r) & (m ^ r) & $80
            a_.op(JOp::Xor, R4, R0, R2);
            a_.op(JOp::Xor, R5, R1, R2);
            a_.op(JOp::And, R4, R4, R5);
            a_.op_imm(JOp::And, R4, R4, kV);
            a_.op(JOp::Or, R3, R3, R4);
            add_nz(R3, R2, R4);
            a_.store_state(kStateA, R2, 1);
            update_ccr(kH | kV | kN | kZ | kC, R3);
            break;
        }
        case Op::Sub:
        case Op::Cmp:
        case Op::Sbc:
        case Op::Cpx: {
            if (in.op == Op::Sbc) {
                carry_in(R4);
            } else {
                a_.move_imm(R4, 0);
            }
            a_.op(JOp::Sub, R2, R0, R1);
            a_.op(JOp::Sub, R2, R2, R4);
            // C: the borrow (bit 8 of the 32-bit difference)
            a_.op_imm(JOp::Shr, R3, R2, 8);
            a_.op_imm(JOp::And, R3, R3, 1);
            a_.op_imm(JOp::And, R2, R2, 0xFF);
            // V: (a ^ m) & (a ^ r) & $80
            a_.op(JOp::Xor, R4, R0, R1);
            a_.op(JOp::Xor, R5, R0, R2);
            a_.op(JOp::And, R4, R4, R5);
            a_.op_imm(JOp::And, R4, R4, kV);
            a_.op(JOp::Or, R3, R3, R4);
            add_nz(R3, R2, R4);
            if (in.op == Op::Sub || in.op == Op::Sbc) {
                a_.store_state(kStateA, R2, 1);
            }
            update_ccr(kV | kN | kZ | kC, R3);
            break;
        }
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
            break;
    }
}

void Translator::helper(const Insn& in, uint32_t remaining) {
    // execute_insn needs the instruction's own pc in the state
    a_.store_state_imm(kStatePc, pc_, 2);
    a_.call_step(R0, &in, remaining);
    const size_t go_on = a_.new_label();
    a_.branch_if(JCond::Ne, R0, 0, go_on);
    a_.leave();
    a_.bind(go_on);
}

bool Translator::instruction(const Insn& in, uint32_t remaining, bool last) {
    insn_ = &in;
    const bool check_irq = std::exchange(check_irq_after_, false);

    // CLI last in its block: the helper sets the interrupt delay in the state for the engine loop
    if (is_helper(in.op) || (in.op == Op::Cli && last)) {
        // execute_insn leaves the state's pc right whatever happens
        helper(in, remaining);
        pc_ = next();
        if (ends_block(in) || last) {
            a_.leave();
            return false;
        }
        return true;
    }

    a_.add_cycles(in.cycles);
    bool ends = false;

    switch (in.op) {
        // ---------------------------------------------------------------- bits
        case Op::Brset:
        case Op::Brclr:
            a_.move_imm(R2, in.b[0]);
            read(R0, R2);
            a_.op_imm(JOp::Shr, R0, R0, in.bit);
            a_.op_imm(JOp::And, R0, R0, 1);
            update_ccr(kC, R0);
            if (in.op == Op::Brset) {
                a_.move(R1, R0);
            } else {
                a_.op_imm(JOp::Xor, R1, R0, 1);
            }
            branch_on(R1, rel_target(in.b[1]));
            ends = true;
            break;
        case Op::Bset:
        case Op::Bclr:
            a_.move_imm(R2, in.b[0]);
            read(R0, R2);
            if (in.op == Op::Bset) {
                a_.op_imm(JOp::Or, R0, R0, 1u << in.bit);
            } else {
                a_.op_imm(JOp::And, R0, R0, ~(1u << in.bit) & 0xFF);
            }
            write(R2, R0);
            break;

        // ---------------------------------------------------------------- control flow
        case Op::Bsr:
            push_return(next());
            leave_at(rel_target(in.b[0]));
            ends = true;
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
            branch(in);
            ends = true;
            break;
        case Op::Jmp:
            address(R0);
            leave_at_reg(R0);
            ends = true;
            break;
        case Op::Jsr:
            address(R0);  // R0 survives the pushes (a slow push stores the return pc in the state)
            push_return(next());
            leave_at_reg(R0);
            ends = true;
            break;
        case Op::Rts:
            pull(R0);
            pull(R1);
            a_.op_imm(JOp::Shl, R0, R0, 8);
            a_.op(JOp::Or, R0, R0, R1);
            leave_at_reg(R0);
            ends = true;
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
            rmw(in);
            break;

        case Op::Cbeq: {
            uint8_t rel = in.b[1];
            switch (in.mode) {
                case Mode::InhA:
                    a_.move_imm(R1, in.b[0]);
                    a_.load_state(R0, kStateA, 1);
                    break;
                case Mode::InhX:
                    a_.move_imm(R1, in.b[0]);
                    load_x(R0);
                    break;
                case Mode::IxPlus:
                    rel = in.b[0];
                    address(R2);
                    read(R1, R2);
                    increment_hx();
                    a_.load_state(R0, kStateA, 1);
                    break;
                case Mode::Ix1Plus:
                    address(R2);
                    read(R1, R2);
                    increment_hx();
                    a_.load_state(R0, kStateA, 1);
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
                    address(R2);
                    read(R1, R2);
                    a_.load_state(R0, kStateA, 1);
                    break;
            }
            a_.op(JOp::Xor, R0, R0, R1);
            a_.set_if(JCond::Eq, R1, R0, 0);
            branch_on(R1, rel_target(rel));
            ends = true;
            break;
        }

        case Op::Dbnz: {
            uint8_t rel;
            if (in.mode == Mode::InhA) {
                a_.load_state(R0, kStateA, 1);
                a_.op_imm(JOp::Sub, R0, R0, 1);
                a_.op_imm(JOp::And, R0, R0, 0xFF);
                a_.store_state(kStateA, R0, 1);
                rel = in.b[0];
            } else if (in.mode == Mode::InhX) {
                load_x(R0);
                a_.op_imm(JOp::Sub, R0, R0, 1);
                a_.op_imm(JOp::And, R0, R0, 0xFF);
                store_x(R0);
                rel = in.b[0];
            } else {
                address(R2);
                read(R0, R2);
                a_.op_imm(JOp::Sub, R0, R0, 1);
                a_.op_imm(JOp::And, R0, R0, 0xFF);
                write(R2, R0);
                rel = in.mode == Mode::Ix ? in.b[0] : in.b[1];
            }
            a_.set_if(JCond::Ne, R1, R0, 0);
            branch_on(R1, rel_target(rel));
            ends = true;
            break;
        }

        case Op::Mov:
            switch (in.mode) {
                case Mode::DirDir:
                    a_.move_imm(R2, in.b[0]);
                    read(R0, R2);
                    a_.move_imm(R2, in.b[1]);
                    break;
                case Mode::ImmDir:
                    a_.move_imm(R0, in.b[0]);
                    a_.move_imm(R2, in.b[1]);
                    break;
                case Mode::DirIxP:
                    a_.move_imm(R2, in.b[0]);
                    read(R0, R2);
                    a_.load_state(R2, kStateHx, 2);
                    increment_hx();
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
                    a_.load_state(R2, kStateHx, 2);
                    read(R0, R2);
                    increment_hx();
                    a_.move_imm(R2, in.b[0]);
                    break;
            }
            write(R2, R0);
            logic_flags(R0);
            break;

        // ---------------------------------------------------------------- 16-bit index
        case Op::Ldhx:
        case Op::Cphx: {
            if (in.mode == Mode::Imm16) {
                a_.move_imm(R1, in.word());
            } else {
                a_.move_imm(R2, in.b[0]);
                read(R1, R2);
                a_.move_imm(R2, uint32_t(in.b[0]) + 1);
                read(R3, R2);
                a_.op_imm(JOp::Shl, R1, R1, 8);
                a_.op(JOp::Or, R1, R1, R3);
            }
            if (in.op == Op::Ldhx) {
                a_.store_state(kStateHx, R1, 2);
                // V=0, N = bit 15, Z
                a_.op_imm(JOp::Shr, R3, R1, 13);
                a_.op_imm(JOp::And, R3, R3, kN);
                a_.set_if(JCond::Eq, R4, R1, 0);
                a_.op_imm(JOp::Shl, R4, R4, 1);
                a_.op(JOp::Or, R3, R3, R4);
                update_ccr(kV | kN | kZ, R3);
            } else {
                a_.load_state(R0, kStateHx, 2);
                a_.op(JOp::Sub, R2, R0, R1);
                a_.op_imm(JOp::Shr, R3, R2, 16);
                a_.op_imm(JOp::And, R3, R3, 1);  // C: the borrow
                a_.op_imm(JOp::And, R2, R2, 0xFFFF);
                a_.op(JOp::Xor, R4, R0, R1);
                a_.op(JOp::Xor, R5, R0, R2);
                a_.op(JOp::And, R4, R4, R5);
                a_.op_imm(JOp::Shr, R4, R4, 8);
                a_.op_imm(JOp::And, R4, R4, kV);
                a_.op(JOp::Or, R3, R3, R4);
                a_.op_imm(JOp::Shr, R4, R2, 13);
                a_.op_imm(JOp::And, R4, R4, kN);
                a_.op(JOp::Or, R3, R3, R4);
                a_.set_if(JCond::Eq, R4, R2, 0);
                a_.op_imm(JOp::Shl, R4, R4, 1);
                a_.op(JOp::Or, R3, R3, R4);
                update_ccr(kV | kN | kZ | kC, R3);
            }
            break;
        }
        case Op::Sthx:
            a_.load_state(R1, kStateHx, 2);
            a_.op_imm(JOp::Shr, R0, R1, 8);
            a_.move_imm(R2, in.b[0]);
            write(R2, R0);
            a_.op_imm(JOp::And, R0, R1, 0xFF);
            a_.move_imm(R2, uint32_t(in.b[0]) + 1);
            write(R2, R0);
            a_.op_imm(JOp::Shr, R3, R1, 13);
            a_.op_imm(JOp::And, R3, R3, kN);
            a_.set_if(JCond::Eq, R4, R1, 0);
            a_.op_imm(JOp::Shl, R4, R4, 1);
            a_.op(JOp::Or, R3, R3, R4);
            update_ccr(kV | kN | kZ, R3);
            break;
        case Op::Aix:
        case Op::Ais: {
            const int32_t field = in.op == Op::Aix ? kStateHx : kStateSp;
            a_.load_state(R0, field, 2);
            a_.op_imm(JOp::Add, R0, R0, uint32_t(int32_t(sext8(in.b[0]))));
            a_.op_imm(JOp::And, R0, R0, 0xFFFF);
            a_.store_state(field, R0, 2);
            break;
        }

        // ---------------------------------------------------------------- register / memory
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
        case Op::Ldx:
        case Op::Stx:
            alu(in);
            break;

        // ---------------------------------------------------------------- inherent
        case Op::Mul:
            a_.load_state(R0, kStateA, 1);
            load_x(R1);
            a_.op(JOp::Mul, R2, R0, R1);
            a_.op_imm(JOp::And, R0, R2, 0xFF);
            a_.store_state(kStateA, R0, 1);
            a_.op_imm(JOp::Shr, R0, R2, 8);
            store_x(R0);
            a_.move_imm(R3, 0);
            update_ccr(kH | kC, R3);
            break;
        case Op::Pula:
            pull(R0);
            a_.store_state(kStateA, R0, 1);
            break;
        case Op::Psha:
            a_.load_state(R0, kStateA, 1);
            push(R0);
            break;
        case Op::Pulx:
            pull(R0);
            store_x(R0);
            break;
        case Op::Pshx:
            load_x(R0);
            push(R0);
            break;
        case Op::Pulh:
            pull(R0);
            a_.load_state(R1, kStateHx, 2);
            a_.op_imm(JOp::And, R1, R1, 0xFF);
            a_.op_imm(JOp::Shl, R0, R0, 8);
            a_.op(JOp::Or, R1, R1, R0);
            a_.store_state(kStateHx, R1, 2);
            break;
        case Op::Pshh:
            a_.load_state(R0, kStateHx, 2);
            a_.op_imm(JOp::Shr, R0, R0, 8);
            push(R0);
            break;
        case Op::Clrh:
            load_x(R0);
            a_.store_state(kStateHx, R0, 2);
            break;
        case Op::Txs:
            a_.load_state(R0, kStateHx, 2);
            a_.op_imm(JOp::Sub, R0, R0, 1);
            a_.op_imm(JOp::And, R0, R0, 0xFFFF);
            a_.store_state(kStateSp, R0, 2);
            break;
        case Op::Tsx:
            a_.load_state(R0, kStateSp, 2);
            a_.op_imm(JOp::Add, R0, R0, 1);
            a_.op_imm(JOp::And, R0, R0, 0xFFFF);
            a_.store_state(kStateHx, R0, 2);
            break;
        case Op::Tax:
            a_.load_state(R0, kStateA, 1);
            store_x(R0);
            break;
        case Op::Txa:
            load_x(R0);
            a_.store_state(kStateA, R0, 1);
            break;
        case Op::Sei:
            // masks interrupts: nothing the engine loop must see
            a_.load_state(R0, kStateCcr, 1);
            a_.op_imm(JOp::Or, R0, R0, kI);
            a_.store_state(kStateCcr, R0, 1);
            break;
        case Op::Cli:
            // not last in the block: the next instruction runs here undisturbed (CLI's one-instruction delay), and
            // after it the block leaves if an interrupt is pending (check_irq_after_)
            a_.load_state(R0, kStateCcr, 1);
            a_.op_imm(JOp::And, R0, R0, ~kI & 0xFF);
            a_.store_state(kStateCcr, R0, 1);
            check_irq_after_ = true;
            break;
        case Op::Clc:
        case Op::Sec:
            a_.move_imm(R3, in.op == Op::Sec ? kC : 0);
            update_ccr(kC, R3);
            break;
        case Op::Nop:
            break;
        case Op::Illegal:
        case Op::Div:
        case Op::Nsa:
        case Op::Daa:
        case Op::Rti:
        case Op::Swi:
        case Op::Tap:
        case Op::Tpa:
        case Op::Stop:
        case Op::Wait:
        case Op::Rsp:
            throw JitError(std::string("translator: no form for ") + mnemonic(in.op));
    }

    pc_ = next();
    if (ends) {
        return false;
    }

    // after CLI and one more instruction: an interrupt the engine loop would take now ends the block
    if (check_irq) {
        const size_t no_irq = a_.new_label();
        a_.load_irq_lines(R0);
        a_.branch_if(JCond::Eq, R0, 0, no_irq);
        a_.load_state(R1, kStateCcr, 1);
        a_.op_imm(JOp::And, R1, R1, kI);
        a_.branch_if(JCond::Ne, R1, 0, no_irq);
        exit_now(pc_);
        a_.bind(no_irq);
    }

    // a helper access (a device, a flash write, code in RAM) made the engine's view change: leave after this
    // instruction so the interrupt and limit checks run where the interpreter runs them
    const size_t go_on = a_.new_label();
    a_.load_exit(R0);
    a_.branch_if(JCond::Eq, R0, 0, go_on);
    exit_now(pc_);
    a_.bind(go_on);

    if (last) {
        leave_at(pc_);
        return false;
    }
    return true;
}

}  // namespace

bool ends_block(const Insn& in) {
    switch (in.op) {
        case Op::Brset:
        case Op::Brclr:
        case Op::Bsr:
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
        case Op::Jmp:
        case Op::Jsr:
        case Op::Rts:
        case Op::Rti:
        case Op::Swi:
        case Op::Stop:
        case Op::Wait:
        case Op::Illegal:
        case Op::Cbeq:
        case Op::Dbnz:
        // TAP may clear I with the interrupt delay: the engine decides what comes next (CLI and SEI are native)
        case Op::Tap:
            return true;
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
        case Op::Ldx:
        case Op::Stx:
        case Op::Mul:
        case Op::Div:
        case Op::Nsa:
        case Op::Daa:
        case Op::Tpa:
        case Op::Pula:
        case Op::Psha:
        case Op::Pulx:
        case Op::Pshx:
        case Op::Pulh:
        case Op::Pshh:
        case Op::Clrh:
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

bool host_supported() {
#if defined(__aarch64__) || defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<TranslatedBlock> translate(CodeMemory& memory, uint16_t pc, std::vector<Insn> insns) {
    auto block = std::make_unique<TranslatedBlock>();
    block->pc = pc;
    block->insns = std::move(insns);
    for (const Insn& in : block->insns) {
        block->cycles += in.cycles;
        if (is_helper(in.op)) {
            block->helpers++;
        } else {
            block->native++;
        }
    }

    asmjit::CodeHolder code;
    code.init(asmjit::Environment::host());
    std::unique_ptr<BlockAssembler> a = make_host_assembler(&code);
    if (!a) {
        throw JitError("no JIT assembler for this host");
    }

    a->prologue();
    Translator t(*a, pc);
    uint32_t remaining = block->cycles;
    for (size_t k = 0; k < block->insns.size(); k++) {
        const Insn& in = block->insns[k];
        remaining -= in.cycles;
        if (!t.instruction(in, remaining, k + 1 == block->insns.size())) {
            break;
        }
    }

    if (code.flatten() != asmjit::Error::kOk) {
        throw JitError("asmjit flatten failed");
    }
    const size_t size = code.code_size();
    CodeMemory::WritableScope scope = memory.make_writable();
    auto span = scope.allocate(size);
    if (!span) {
        return nullptr;
    }
    std::byte* runs_at = memory.executable_address(span->data());
    if (code.relocate_to_base(uint64_t(reinterpret_cast<uintptr_t>(runs_at))) != asmjit::Error::kOk ||
        code.copy_flattened_data(span->data(), span->size()) != asmjit::Error::kOk) {
        throw JitError("asmjit relocate/copy failed");
    }
    std::memcpy(&block->entry, &runs_at, sizeof(runs_at));
    block->chain.body = runs_at + a->body_offset();
    block->chain.cycles = block->cycles;
    return block;
}

}  // namespace sim::jit
