// The e200 BlockAssembler for AArch64 hosts (asmjit a64). Registers:
//   x19 the Context, x20 the CpuState, x21 the current software-TLB set (callee-saved: they survive helper calls)
//   w9..w14 the six scratch registers; x15..x17 temporaries; x0..x3 helper arguments
#if defined(__aarch64__)

#include <asmjit/a64.h>
#include <asmjit/core.h>

#include <vector>

#include "e200/jit.h"
#include "e200/machine.h"

namespace sim::e200::jit {

namespace {

using namespace asmjit::a64;
using asmjit::imm;

constexpr Gp kContext = x19;
constexpr Gp kState = x20;
constexpr Gp kSoft = x21;

const Gp kScratch[kScratchRegisters] = {w9, w10, w11, w12, w13, w14};

constexpr int32_t kFrameBytes = 48;
constexpr int32_t kSoftRead = int32_t(offsetof(SoftTlb, read));
constexpr int32_t kSoftWrite = int32_t(offsetof(SoftTlb, write));
constexpr int32_t kEntryTag = int32_t(offsetof(SoftTlbEntry, tag));
constexpr int32_t kEntryHost = int32_t(offsetof(SoftTlbEntry, host));

class Aarch64Assembler final : public BlockAssembler {
   public:
    explicit Aarch64Assembler(asmjit::CodeHolder* code) : a_(code) {
    }

    void prologue() override {
        a_.sub(sp, sp, imm(kFrameBytes));
        a_.stp(x19, x20, ptr(sp, 0));
        a_.stp(x21, x30, ptr(sp, 16));
        a_.mov(kContext, x0);
        a_.ldr(kState, ptr(kContext, kCtxState));
        load_soft();
    }

    void leave() override {
        a_.ldp(x21, x30, ptr(sp, 16));
        a_.ldp(x19, x20, ptr(sp, 0));
        a_.add(sp, sp, imm(kFrameBytes));
        a_.ret(x30);
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
        if (bytes == 1) {
            a_.ldrb(kScratch[d], ptr(kState, offset));
        } else {
            a_.ldr(kScratch[d], ptr(kState, offset));
        }
    }
    void store_state(int32_t offset, int s) override {
        a_.str(kScratch[s], ptr(kState, offset));
    }
    void store_state_imm(int32_t offset, uint32_t value) override {
        load_imm(w15, value);
        a_.str(w15, ptr(kState, offset));
    }
    void set_cycles(uint32_t n) override {
        a_.ldr(x15, ptr(kContext, kCtxStartCycles));
        if (n < 4096) {
            a_.add(x15, x15, imm(n));
        } else {
            load_imm(w16, n);
            a_.add(x15, x15, x16);
        }
        a_.str(x15, ptr(kState, kStateCycles));
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
        const Gp& rd = kScratch[d];
        const Gp& rx = kScratch[x];
        switch (op) {
            case JOp::Shl:
                a_.lsl(rd, rx, imm(value & 31));
                return;
            case JOp::Shr:
                a_.lsr(rd, rx, imm(value & 31));
                return;
            case JOp::Sar:
                a_.asr(rd, rx, imm(value & 31));
                return;
            case JOp::Rotl:
                a_.ror(rd, rx, imm((32 - (value & 31)) & 31));
                return;
            case JOp::Add:
            case JOp::Sub:
                if (value < 4096) {
                    if (op == JOp::Add) {
                        a_.add(rd, rx, imm(value));
                    } else {
                        a_.sub(rd, rx, imm(value));
                    }
                    return;
                }
                break;
            case JOp::And:
            case JOp::Or:
            case JOp::Xor:
            case JOp::Mul:
                break;
        }
        load_imm(w17, value);
        emit_op(op, rd, rx, w17);
    }
    void not_(int d, int s) override {
        a_.mvn(kScratch[d], kScratch[s]);
    }
    void set_if(JCond cond, int d, int x, int y) override {
        a_.cmp(kScratch[x], kScratch[y]);
        a_.cset(kScratch[d], cond_of(cond));
    }
    void set_if_imm(JCond cond, int d, int x, uint32_t value) override {
        compare(x, value);
        a_.cset(kScratch[d], cond_of(cond));
    }
    void branch_if(JCond cond, int x, int y, size_t label) override {
        a_.cmp(kScratch[x], kScratch[y]);
        a_.b(cond_of(cond), labels_[label]);
    }
    void branch_if_imm(JCond cond, int x, uint32_t value, size_t label) override {
        if (value == 0 && cond == JCond::Eq) {
            a_.cbz(kScratch[x], labels_[label]);
            return;
        }
        if (value == 0 && cond == JCond::Ne) {
            a_.cbnz(kScratch[x], labels_[label]);
            return;
        }
        compare(x, value);
        a_.b(cond_of(cond), labels_[label]);
    }
    void count_leading_zeros(int d, int s) override {
        a_.clz(kScratch[d], kScratch[s]);
    }

    void mem_load(int d, int ea, int size, size_t slow) override {
        const asmjit::Label& to_slow = labels_[slow];
        lookup(ea, size, kSoftRead, to_slow);
        // x16 = the page's host address, x17 = the offset in it
        switch (size) {
            case 1:
                a_.ldrb(kScratch[d], ptr(x16, x17));
                break;
            case 2:
                a_.ldrh(kScratch[d], ptr(x16, x17));
                a_.rev16(kScratch[d], kScratch[d]);
                break;
            default:
                a_.ldr(kScratch[d], ptr(x16, x17));
                a_.rev(kScratch[d], kScratch[d]);
                break;
        }
    }
    void mem_store(int ea, int value, int size, size_t slow) override {
        const asmjit::Label& to_slow = labels_[slow];
        a_.ldrb(w16, ptr(kState, kStateReserved));
        a_.cbnz(w16, to_slow);
        lookup(ea, size, kSoftWrite, to_slow);
        a_.add(x16, x16, x17);
        switch (size) {
            case 1:
                a_.strb(kScratch[value], ptr(x16));
                break;
            case 2:
                a_.rev16(w17, kScratch[value]);
                a_.strh(w17, ptr(x16));
                break;
            default:
                a_.rev(w17, kScratch[value]);
                a_.str(w17, ptr(x16));
                break;
        }
    }

    void call_step(int d, const Insn* insn, uint32_t pc, uint32_t k) override {
        a_.mov(x0, kContext);
        load_imm64(x1, uint64_t(reinterpret_cast<uintptr_t>(insn)));
        load_imm(w2, pc);
        load_imm(w3, k);
        a_.ldr(x16, ptr(kContext, kCtxStep));
        a_.blr(x16);
        a_.mov(kScratch[d], w0);
        load_soft();
    }

   private:
    // x15 = the software-TLB entry for `ea` (of the set at `kind`); to `slow` unless its tag is ea's page and the
    // access stays in the page. Leaves x16 = the page's host address, x17 = ea's offset in the page.
    void lookup(int ea, int size, int32_t kind, const asmjit::Label& slow) {
        const Gp& r = kScratch[ea];
        a_.ubfx(w15, r, imm(12), imm(8));
        a_.add(x15, kSoft, x15, lsl(4));
        a_.ldr(w16, ptr(x15, kind + kEntryTag));
        a_.and_(w17, r, imm(0xFFFFF000u));
        a_.cmp(w16, w17);
        a_.b_ne(slow);
        a_.and_(w17, r, imm(0xFFFu));
        a_.cmp(w17, imm(uint32_t(0x1000 - size)));
        a_.b_hi(slow);
        a_.ldr(x16, ptr(x15, kind + kEntryHost));
    }

    void load_soft() {
        a_.ldr(kSoft, ptr(kContext, kCtxSoft));
        a_.ldr(kSoft, ptr(kSoft));
    }

    static CondCode cond_of(JCond c) {
        switch (c) {
            case JCond::Eq: return CondCode::kEQ;
            case JCond::Ne: return CondCode::kNE;
            case JCond::Lt: return CondCode::kLT;
            case JCond::Gt: return CondCode::kGT;
            case JCond::Le: return CondCode::kLE;
            case JCond::Ge: return CondCode::kGE;
            case JCond::Ltu: return CondCode::kLO;
            case JCond::Gtu: return CondCode::kHI;
        }
        return CondCode::kEQ;
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
            case JOp::Add: a_.add(d, x, y); break;
            case JOp::Sub: a_.sub(d, x, y); break;
            case JOp::And: a_.and_(d, x, y); break;
            case JOp::Or: a_.orr(d, x, y); break;
            case JOp::Xor: a_.eor(d, x, y); break;
            case JOp::Shl: a_.lsl(d, x, y); break;
            case JOp::Shr: a_.lsr(d, x, y); break;
            case JOp::Sar: a_.asr(d, x, y); break;
            case JOp::Mul: a_.mul(d, x, y); break;
            case JOp::Rotl:
                // rotate left by y = rotate right by -y (mod 32)
                a_.neg(w15, y);
                a_.ror(d, x, w15);
                break;
        }
    }

    Assembler a_;
    std::vector<asmjit::Label> labels_;
};

}  // namespace

std::unique_ptr<BlockAssembler> make_host_assembler(void* code_holder) {
    return std::make_unique<Aarch64Assembler>(static_cast<asmjit::CodeHolder*>(code_holder));
}

bool host_supported() {
    return true;
}

}  // namespace sim::e200::jit

#else

#include "e200/jit.h"

namespace sim::e200::jit {

std::unique_ptr<BlockAssembler> make_host_assembler(void*) {
    return nullptr;
}

bool host_supported() {
    return false;
}

}  // namespace sim::e200::jit

#endif
