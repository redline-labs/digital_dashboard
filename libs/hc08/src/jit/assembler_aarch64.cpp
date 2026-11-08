// The BlockAssembler for AArch64 hosts (asmjit a64). Registers:
//   x19 the JitContext, x20 the CpuState, x21 guest memory, x22 the region tags, x23 the code-page flags
//   w24 A, w25 HX, w26 SP, w27 CCR, x28 the cycle count (the guest registers, callee-saved: they survive helper calls)
//   w9..w14 the six scratch registers; x15..x17 temporaries; x0..x3 helper arguments
#if defined(__aarch64__)

#include <asmjit/a64.h>
#include <asmjit/core.h>

#include <vector>

#include "hc08/jit/block_assembler.h"
#include "hc08/jit/native_abi.h"
#include "hc08/memmap.h"

namespace sim::jit {

namespace {

using namespace asmjit::a64;
using asmjit::imm;

constexpr Gp kContext = x19;
constexpr Gp kState = x20;
constexpr Gp kMemory = x21;
constexpr Gp kRegions = x22;
constexpr Gp kCodePages = x23;
constexpr Gp kA = w24;
constexpr Gp kHx = w25;
constexpr Gp kSp = w26;
constexpr Gp kCcr = w27;
constexpr Gp kCycles = x28;

const Gp kScratch[kScratchRegisters] = {w9, w10, w11, w12, w13, w14};

constexpr int32_t kFrameBytes = 96;

class Aarch64Assembler final : public BlockAssembler {
   public:
    explicit Aarch64Assembler(asmjit::CodeHolder* code) : code_(code), a_(code) {
    }

    void prologue() override {
        a_.sub(sp, sp, imm(kFrameBytes));
        a_.stp(x19, x20, ptr(sp, 0));
        a_.stp(x21, x22, ptr(sp, 16));
        a_.stp(x23, x24, ptr(sp, 32));
        a_.stp(x25, x26, ptr(sp, 48));
        a_.stp(x27, x28, ptr(sp, 64));
        a_.str(x30, ptr(sp, 80));
        a_.mov(kContext, x0);
        a_.ldr(kState, ptr(kContext, kCtxState));
        a_.ldr(kMemory, ptr(kContext, kCtxMemory));
        a_.ldr(kRegions, ptr(kContext, kCtxRegions));
        a_.ldr(kCodePages, ptr(kContext, kCtxCodePages));
        reload_guest();
        body_ = a_.new_label();
        a_.bind(body_);
    }

    uint64_t body_offset() override {
        return code_->label_offset_from_base(body_);
    }

    void leave() override {
        flush_guest();
        a_.ldr(x30, ptr(sp, 80));
        a_.ldp(x27, x28, ptr(sp, 64));
        a_.ldp(x25, x26, ptr(sp, 48));
        a_.ldp(x23, x24, ptr(sp, 32));
        a_.ldp(x21, x22, ptr(sp, 16));
        a_.ldp(x19, x20, ptr(sp, 0));
        a_.add(sp, sp, imm(kFrameBytes));
        a_.ret(x30);
    }

    void dispatch_imm(uint16_t pc) override {
        load_imm(w15, pc);
        dispatch();
    }
    void dispatch_reg(int pc) override {
        a_.mov(w15, kScratch[pc]);
        dispatch();
    }

    size_t new_label() override {
        labels_.push_back(a_.new_label());
        return labels_.size() - 1;
    }
    void bind(size_t label) override {
        a_.bind(labels_[label]);
    }
    void jump(size_t label) override {
        a_.b(labels_[label]);
    }

    void load_state(int d, int32_t offset, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            a_.mov(kScratch[d], *cached);
        } else if (bytes == 1) {
            a_.ldrb(kScratch[d], ptr(kState, offset));
        } else {
            a_.ldrh(kScratch[d], ptr(kState, offset));
        }
    }
    void store_state(int32_t offset, int s, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            a_.mov(*cached, kScratch[s]);
        } else if (bytes == 1) {
            a_.strb(kScratch[s], ptr(kState, offset));
        } else {
            a_.strh(kScratch[s], ptr(kState, offset));
        }
    }
    void store_state_imm(int32_t offset, uint32_t value, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            load_imm(*cached, value);
            return;
        }
        load_imm(w15, value);
        if (bytes == 1) {
            a_.strb(w15, ptr(kState, offset));
        } else {
            a_.strh(w15, ptr(kState, offset));
        }
    }
    void add_cycles(uint32_t n) override {
        a_.add(kCycles, kCycles, imm(n));
    }

    void move_imm(int d, uint32_t value) override {
        load_imm(kScratch[d], value);
    }
    void move(int d, int s) override {
        if (d != s) {
            a_.mov(kScratch[d], kScratch[s]);
        }
    }

    void op(JOp op, int d, int x, int y) override {
        emit_op(op, kScratch[d], kScratch[x], kScratch[y]);
    }
    void op_imm(JOp op, int d, int x, uint32_t value) override {
        if (op == JOp::Shl) {
            a_.lsl(kScratch[d], kScratch[x], imm(value & 31));
            return;
        }
        if (op == JOp::Shr) {
            a_.lsr(kScratch[d], kScratch[x], imm(value & 31));
            return;
        }
        if ((op == JOp::Add || op == JOp::Sub) && value < 4096) {
            if (op == JOp::Add) {
                a_.add(kScratch[d], kScratch[x], imm(value));
            } else {
                a_.sub(kScratch[d], kScratch[x], imm(value));
            }
            return;
        }
        load_imm(w17, value);
        emit_op(op, kScratch[d], kScratch[x], w17);
    }

    void set_if(JCond cond, int d, int x, uint32_t value) override {
        compare(x, value);
        a_.cset(kScratch[d], cond_of(cond));
    }
    void branch_if(JCond cond, int x, uint32_t value, size_t label) override {
        if (value == 0 && (cond == JCond::Eq || cond == JCond::Ne)) {
            if (cond == JCond::Eq) {
                a_.cbz(kScratch[x], labels_[label]);
            } else {
                a_.cbnz(kScratch[x], labels_[label]);
            }
            return;
        }
        compare(x, value);
        branch(cond, labels_[label]);
    }

    void read8(int d, int addr, uint16_t pc) override {
        const asmjit::Label slow = a_.new_label();
        const asmjit::Label done = a_.new_label();
        const Gp address = kScratch[addr].x();
        a_.ldrb(w15, ptr(kRegions, address));
        a_.cmp(w15, imm(uint32_t(Region::Device)));
        a_.b_eq(slow);
        a_.ldrb(kScratch[d], ptr(kMemory, address));
        a_.b(done);

        a_.bind(slow);
        store_pc(pc);
        flush_guest();
        spill();
        a_.mov(x0, kContext);
        a_.mov(w1, kScratch[addr]);
        load_imm(w2, pc);
        a_.ldr(x16, ptr(kContext, kCtxRead));
        a_.blr(x16);
        reload();
        a_.mov(kScratch[d], w0);
        a_.bind(done);
    }

    void write8(int addr, int value, uint16_t pc) override {
        const asmjit::Label slow = a_.new_label();
        const asmjit::Label done = a_.new_label();
        const Gp address = kScratch[addr].x();
        a_.ldrb(w15, ptr(kRegions, address));
        a_.cmp(w15, imm(uint32_t(Region::Ram)));
        a_.b_ne(slow);
        a_.lsr(w16, kScratch[addr], imm(8));
        a_.ldrb(w16, ptr(kCodePages, x16));
        a_.cbnz(w16, slow);
        a_.strb(kScratch[value], ptr(kMemory, address));
        a_.b(done);

        a_.bind(slow);
        store_pc(pc);
        flush_guest();
        spill();
        a_.mov(x0, kContext);
        a_.mov(w1, kScratch[addr]);
        a_.mov(w2, kScratch[value]);
        load_imm(w3, pc);
        a_.ldr(x16, ptr(kContext, kCtxWrite));
        a_.blr(x16);
        reload();
        a_.bind(done);
    }

    void load_exit(int d) override {
        a_.ldr(kScratch[d], ptr(kContext, kCtxExit));
    }
    void load_irq_lines(int d) override {
        a_.ldr(x15, ptr(kContext, kCtxIrqLines));
        a_.ldr(kScratch[d], ptr(x15));
    }

    void call_step(int d, const Insn* insn, uint32_t remaining) override {
        flush_guest();
        spill();
        a_.mov(x0, kContext);
        load_imm64(x1, uint64_t(reinterpret_cast<uintptr_t>(insn)));
        load_imm(w2, remaining);
        a_.ldr(x16, ptr(kContext, kCtxStep));
        a_.blr(x16);
        reload();
        reload_guest();
        a_.mov(kScratch[d], w0);
    }

   private:
    static CondCode cond_of(JCond c) {
        switch (c) {
            case JCond::Eq:
                return CondCode::kEQ;
            case JCond::Ne:
                return CondCode::kNE;
            case JCond::Ltu:
                return CondCode::kLO;
            case JCond::Geu:
                return CondCode::kHS;
            case JCond::Gtu:
                return CondCode::kHI;
            case JCond::Leu:
                return CondCode::kLS;
        }
        return CondCode::kEQ;
    }

    void branch(JCond cond, const asmjit::Label& l) {
        switch (cond) {
            case JCond::Eq:
                a_.b_eq(l);
                break;
            case JCond::Ne:
                a_.b_ne(l);
                break;
            case JCond::Ltu:
                a_.b_lo(l);
                break;
            case JCond::Geu:
                a_.b_hs(l);
                break;
            case JCond::Gtu:
                a_.b_hi(l);
                break;
            case JCond::Leu:
                a_.b_ls(l);
                break;
        }
    }

    const Gp* guest(int32_t offset) const {
        switch (offset) {
            case kStateA:
                return &kA;
            case kStateHx:
                return &kHx;
            case kStateSp:
                return &kSp;
            case kStateCcr:
                return &kCcr;
            default:
                return nullptr;
        }
    }

    void reload_guest() {
        a_.ldrb(kA, ptr(kState, kStateA));
        a_.ldrh(kHx, ptr(kState, kStateHx));
        a_.ldrh(kSp, ptr(kState, kStateSp));
        a_.ldrb(kCcr, ptr(kState, kStateCcr));
        a_.ldr(kCycles, ptr(kState, kStateCycles));
    }
    void flush_guest() {
        a_.strb(kA, ptr(kState, kStateA));
        a_.strh(kHx, ptr(kState, kStateHx));
        a_.strh(kSp, ptr(kState, kStateSp));
        a_.strb(kCcr, ptr(kState, kStateCcr));
        a_.str(kCycles, ptr(kState, kStateCycles));
    }
    void store_pc(uint16_t pc) {
        load_imm(w15, pc);
        a_.strh(w15, ptr(kState, kStatePc));
    }

    // the next pc in w15: chain or leave
    void dispatch() {
        const asmjit::Label out = a_.new_label();
        const asmjit::Label go = a_.new_label();
        a_.strh(w15, ptr(kState, kStatePc));
        a_.ldr(w16, ptr(kContext, kCtxExit));
        a_.cbnz(w16, out);
        a_.ldr(x16, ptr(kContext, kCtxTable));
        a_.ldr(x16, ptr(x16, x15, lsl(3)));
        a_.cbz(x16, out);
        a_.ldr(x17, ptr(x16, kChainCycles));
        a_.add(x17, x17, kCycles);
        a_.ldr(x9, ptr(kContext, kCtxLimit));
        a_.ldr(x9, ptr(x9));
        a_.cmp(x17, x9);
        a_.b_hi(out);
        a_.ldr(x9, ptr(kContext, kCtxIrqLines));
        a_.ldr(w9, ptr(x9));
        a_.cbz(w9, go);
        a_.tst(kCcr, imm(ccr::kI));
        a_.b_eq(out);
        a_.bind(go);
        a_.ldr(x16, ptr(x16, kChainBody));
        a_.br(x16);
        a_.bind(out);
        leave();
    }

    void load_imm(const Gp& d, uint32_t value) {
        a_.movz(d, imm(value & 0xFFFF));
        if (value >> 16) {
            a_.movk(d, imm(value >> 16), lsl(16));
        }
    }
    void load_imm64(const Gp& d, uint64_t value) {
        a_.movz(d, imm(value & 0xFFFF));
        for (int shift = 16; shift < 64; shift += 16) {
            const uint64_t part = (value >> shift) & 0xFFFF;
            if (part) {
                a_.movk(d, imm(part), lsl(uint32_t(shift)));
            }
        }
    }
    void compare(int x, uint32_t value) {
        if (value < 4096) {
            a_.cmp(kScratch[x], imm(value));
        } else {
            load_imm(w17, value);
            a_.cmp(kScratch[x], w17);
        }
    }
    void emit_op(JOp op, const Gp& d, const Gp& x, const Gp& y) {
        switch (op) {
            case JOp::Add:
                a_.add(d, x, y);
                break;
            case JOp::Sub:
                a_.sub(d, x, y);
                break;
            case JOp::And:
                a_.and_(d, x, y);
                break;
            case JOp::Or:
                a_.orr(d, x, y);
                break;
            case JOp::Xor:
                a_.eor(d, x, y);
                break;
            case JOp::Shl:
                a_.lsl(d, x, y);
                break;
            case JOp::Shr:
                a_.lsr(d, x, y);
                break;
            case JOp::Mul:
                a_.mul(d, x, y);
                break;
        }
    }
    void spill() {
        for (int k = 0; k < kScratchRegisters; k += 2) {
            a_.stp(kScratch[k], kScratch[k + 1], ptr(kContext, kCtxSpill + 4 * k));
        }
    }
    void reload() {
        for (int k = 0; k < kScratchRegisters; k += 2) {
            a_.ldp(kScratch[k], kScratch[k + 1], ptr(kContext, kCtxSpill + 4 * k));
        }
    }

    asmjit::CodeHolder* code_;
    Assembler a_;
    asmjit::Label body_;
    std::vector<asmjit::Label> labels_;
};

}  // namespace

std::unique_ptr<BlockAssembler> make_host_assembler(void* code_holder) {
    return std::make_unique<Aarch64Assembler>(static_cast<asmjit::CodeHolder*>(code_holder));
}

}  // namespace sim::jit

#endif
