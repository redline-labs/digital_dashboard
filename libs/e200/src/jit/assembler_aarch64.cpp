// The e200 BlockAssembler for AArch64 hosts (asmjit a64). Registers:
//   x19 the Context, x20 the CpuState, x21 the current software-TLB set (callee-saved: they survive helper calls)
//   w22..w28 the block's cached guest registers (callee-saved too)
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

constexpr int32_t kFrameBytes = 96;
const Gp kCached[kCachedRegisters] = {w22, w23, w24, w25, w26, w27, w28};
constexpr int32_t kSoftRead = int32_t(offsetof(SoftTlb, read));
constexpr int32_t kSoftWrite = int32_t(offsetof(SoftTlb, write));
constexpr int32_t kSoftExec = int32_t(offsetof(SoftTlb, exec));
constexpr int32_t kEntryPhys = int32_t(offsetof(SoftTlbEntry, phys));
constexpr int32_t kEntryTag = int32_t(offsetof(SoftTlbEntry, tag));
constexpr int32_t kEntryHost = int32_t(offsetof(SoftTlbEntry, host));

class Aarch64Assembler final : public BlockAssembler {
   public:
    explicit Aarch64Assembler(asmjit::CodeHolder* code) : code_(code), a_(code) {
    }

    void set_cached(const std::vector<int32_t>& offsets) override {
        cached_ = offsets;
        if (cached_.size() > size_t(kCachedRegisters)) {
            cached_.resize(size_t(kCachedRegisters));
        }
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
        load_soft();
    }

    void leave() override {
        flush();
        leave_raw();
    }

    void leave_raw() override {
        a_.ldr(x30, ptr(sp, 80));
        a_.ldp(x27, x28, ptr(sp, 64));
        a_.ldp(x25, x26, ptr(sp, 48));
        a_.ldp(x23, x24, ptr(sp, 32));
        a_.ldp(x21, x22, ptr(sp, 16));
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
        if (const Gp* c = cached(offset); c && bytes == 4) {
            a_.mov(kScratch[d], *c);
        } else if (bytes == 1) {
            a_.ldrb(kScratch[d], ptr(kState, offset));
        } else {
            a_.ldr(kScratch[d], ptr(kState, offset));
        }
    }
    void store_state(int32_t offset, int s) override {
        if (const Gp* c = cached(offset)) {
            a_.mov(*c, kScratch[s]);
        } else {
            a_.str(kScratch[s], ptr(kState, offset));
        }
    }
    void store_state_imm(int32_t offset, uint32_t value) override {
        if (const Gp* c = cached(offset)) {
            load_imm(*c, value);
            return;
        }
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
    void compare_field(unsigned field, int x, int y, uint32_t value, bool is_signed) override {
        if (y >= 0) {
            a_.cmp(kScratch[x], kScratch[y]);
        } else {
            compare(x, value);
        }
        a_.cset(w15, is_signed ? CondCode::kLT : CondCode::kLO);
        a_.cset(w16, is_signed ? CondCode::kGT : CondCode::kHI);
        a_.cset(w17, CondCode::kEQ);
        load_word(w9, kStateXer);
        a_.lsr(w9, w9, imm(31));
        a_.orr(w9, w9, w17, lsl(1));
        a_.orr(w9, w9, w16, lsl(2));
        a_.orr(w9, w9, w15, lsl(3));
        const uint32_t shift = 28 - 4 * field;
        if (const Gp* cr = cached(kStateCr)) {
            a_.bfi(*cr, w9, imm(shift), imm(4));
        } else {
            a_.ldr(w10, ptr(kState, kStateCr));
            a_.bfi(w10, w9, imm(shift), imm(4));
            a_.str(w10, ptr(kState, kStateCr));
        }
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
        flush();
        a_.mov(x0, kContext);
        load_imm64(x1, uint64_t(reinterpret_cast<uintptr_t>(insn)));
        load_imm(w2, pc);
        load_imm(w3, k);
        a_.ldr(x16, ptr(kContext, kCtxStep));
        a_.blr(x16);
        a_.mov(kScratch[d], w0);
        load_soft();
        reload();
    }

    void call_spe(int d, SpeFunction fn, int x, int y) override {
        a_.mov(w0, kScratch[x]);
        a_.mov(w1, kScratch[y]);
        a_.add(x2, kState, imm(kStateSpefscr));
        load_imm64(x16, uint64_t(reinterpret_cast<uintptr_t>(fn)));
        a_.blr(x16);
        a_.mov(kScratch[d], w0);
    }

    void bind_body() override {
        body_ = a_.new_label();
        loop_ = a_.new_label();
        a_.align(asmjit::AlignMode::kCode, 64);
        a_.bind(body_);
        reload();
        a_.bind(loop_);
    }
    uint64_t body_offset() override {
        return code_->label_offset_from_base(body_);
    }
    void set_block(uint32_t ea, uint32_t pa_page) override {
        block_ea_ = ea;
        block_pa_page_ = pa_page;
    }
    void chain_imm(uint32_t pc, uint32_t executed) override {
        load_imm(w15, pc);
        if (pc == block_ea_) {
            chain_self(executed);
        } else {
            chain(executed, (pc >> 12) == (block_ea_ >> 12));
        }
    }
    void chain_reg(int pc, uint32_t executed) override {
        a_.mov(w15, kScratch[pc]);
        chain(executed, false);
    }

   private:
    // The next pc in w15: store it and the cycle count; enter the next block's body when the table has it for this pc
    // and this physical page and it fits the budget, else leave. Uses x9..x17 (the block is over).
    // a jump to this block's own start (a loop): only the budget to check
    void chain_self(uint32_t executed) {
        const asmjit::Label out = a_.new_label();
        a_.str(w15, ptr(kState, kStatePc));
        a_.ldr(x16, ptr(kContext, kCtxStartCycles));
        add_imm64(x16, executed);
        a_.str(x16, ptr(kContext, kCtxStartCycles));
        a_.str(x16, ptr(kState, kStateCycles));
        add_imm64_to(x12, x16, executed);  // this block's length is `executed`: it ends at its jump back
        a_.ldr(x13, ptr(kContext, kCtxBudget));
        a_.cmp(x12, x13);
        a_.b_hi(out);
        a_.str(x12, ptr(kContext, kCtxBlockEnd));
        a_.b(loop_);  // the cached registers stay where they are
        a_.bind(out);
        leave();
    }

    void add_imm64_to(const Gp& d, const Gp& s, uint32_t n) {
        if (n < 4096) {
            a_.add(d, s, imm(n));
        } else {
            load_imm(w9, n);
            a_.add(d, s, x9);
        }
    }

    void chain(uint32_t executed, bool same_page) {
        const asmjit::Label out = a_.new_label();
        flush();
        a_.str(w15, ptr(kState, kStatePc));
        a_.ldr(x16, ptr(kContext, kCtxStartCycles));
        add_imm64(x16, executed);
        a_.str(x16, ptr(kContext, kCtxStartCycles));
        a_.str(x16, ptr(kState, kStateCycles));
        // the table's entry for pc
        a_.ubfx(w17, w15, imm(2), imm(kChainTableBits));
        a_.ldr(x9, ptr(kContext, kCtxTable));
        a_.ldr(x17, ptr(x9, x17, lsl(3)));
        a_.cbz(x17, out);
        a_.ldr(w9, ptr(x17, kChainEa));
        a_.cmp(w9, w15);
        a_.b_ne(out);
        if (same_page) {
            // this block's page, checked when it was entered: the next block must come from the same physical page
            a_.ldr(w12, ptr(x17, kChainPaPage));
            compare_w(w12, block_pa_page_);
            a_.b_ne(out);
        } else {
            // the pc's page translates, now, to the page the block was translated from
            a_.ubfx(w10, w15, imm(12), imm(8));
            a_.add(x10, kSoft, x10, lsl(4));
            a_.ldr(w11, ptr(x10, kSoftExec + kEntryTag));
            a_.and_(w12, w15, imm(0xFFFFF000u));
            a_.cmp(w11, w12);
            a_.b_ne(out);
            a_.ldr(w11, ptr(x10, kSoftExec + kEntryPhys));
            a_.ldr(w12, ptr(x17, kChainPaPage));
            a_.cmp(w12, w11, lsr(12));
            a_.b_ne(out);
        }
        // it ends within the budget
        a_.ldr(w11, ptr(x17, kChainCount));
        a_.add(x12, x16, x11);
        a_.ldr(x13, ptr(kContext, kCtxBudget));
        a_.cmp(x12, x13);
        a_.b_hi(out);
        a_.str(x12, ptr(kContext, kCtxBlockEnd));
        a_.ldr(x9, ptr(x17, kChainBody));
        a_.br(x9);
        a_.bind(out);
        leave_raw();
    }

    void compare_w(const Gp& r, uint32_t value) {
        if (value < 4096) {
            a_.cmp(r, imm(value));
        } else {
            load_imm(w13, value);
            a_.cmp(r, w13);
        }
    }

    void add_imm64(const Gp& d, uint32_t n) {
        if (n < 4096) {
            a_.add(d, d, imm(n));
        } else {
            load_imm(w9, n);
            a_.add(d, d, x9);
        }
    }

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

    void load_word(const Gp& d, int32_t offset) {
        if (const Gp* c = cached(offset)) {
            a_.mov(d, *c);
        } else {
            a_.ldr(d, ptr(kState, offset));
        }
    }

    const Gp* cached(int32_t offset) const {
        for (size_t k = 0; k < cached_.size(); ++k) {
            if (cached_[k] == offset) {
                return &kCached[k];
            }
        }
        return nullptr;
    }
    void flush() {
        for (size_t k = 0; k < cached_.size(); ++k) {
            a_.str(kCached[k], ptr(kState, cached_[k]));
        }
    }
    void reload() {
        for (size_t k = 0; k < cached_.size(); ++k) {
            a_.ldr(kCached[k], ptr(kState, cached_[k]));
        }
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

    asmjit::CodeHolder* code_;
    Assembler a_;
    asmjit::Label body_;
    asmjit::Label loop_;
    std::vector<asmjit::Label> labels_;
    std::vector<int32_t> cached_;
    uint32_t block_ea_ = 0;
    uint32_t block_pa_page_ = 0;
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
