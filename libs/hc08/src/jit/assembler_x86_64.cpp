// The BlockAssembler for x86-64 hosts (asmjit x86, System V ABI). Registers:
//   rbx the JitContext, r15 guest memory
//   r12d A, r13d HX, r14d SP, ebp CCR (callee-saved), rsi the cycle count (caller-saved: written back before and
//   reloaded after every helper call)
//   ecx, r8d, r9d, r10d, r11d, edi the six scratch registers; eax, edx temporaries
// The CpuState pointer is read from the context where it is needed (exits and helper calls only).
#if defined(__x86_64__)

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <vector>

#include "hc08/jit/block_assembler.h"
#include "hc08/jit/native_abi.h"
#include "hc08/memmap.h"

namespace sim::jit {

namespace {

using namespace asmjit::x86;
using asmjit::imm;

const Gp kContext = rbx;
const Gp kMemory = r15;
const Gp kA = r12d;
const Gp kHx = r13d;
const Gp kSp = r14d;
const Gp kCcr = ebp;
const Gp kCycles = rsi;

const Gp kScratch[kScratchRegisters] = {ecx, r8d, r9d, r10d, r11d, edi};

class X86Assembler final : public BlockAssembler {
   public:
    explicit X86Assembler(asmjit::CodeHolder* code) : code_(code), a_(code) {
    }

    void prologue() override {
        // entry: rsp = 8 mod 16; six pushes and 8 bytes make it 0 mod 16 for the helper calls
        a_.push(rbx);
        a_.push(rbp);
        a_.push(r12);
        a_.push(r13);
        a_.push(r14);
        a_.push(r15);
        a_.sub(rsp, imm(8));
        a_.mov(kContext, rdi);
        a_.mov(kMemory, qword_ptr(kContext, kCtxMemory));
        reload_guest();
        body_ = a_.new_label();
        a_.bind(body_);
    }

    uint64_t body_offset() override {
        return code_->label_offset_from_base(body_);
    }

    void leave() override {
        flush_guest();
        a_.add(rsp, imm(8));
        a_.pop(r15);
        a_.pop(r14);
        a_.pop(r13);
        a_.pop(r12);
        a_.pop(rbp);
        a_.pop(rbx);
        a_.ret();
    }

    void dispatch_imm(uint16_t pc) override {
        a_.mov(eax, imm(uint32_t(pc)));
        dispatch();
    }
    void dispatch_reg(int pc) override {
        a_.mov(eax, kScratch[pc]);
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
        a_.jmp(labels_[label]);
    }

    void load_state(int d, int32_t offset, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            a_.mov(kScratch[d], *cached);
            return;
        }
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        a_.movzx(kScratch[d], bytes == 1 ? byte_ptr(rax, offset) : word_ptr(rax, offset));
    }
    void store_state(int32_t offset, int s, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            a_.mov(*cached, kScratch[s]);
            return;
        }
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        if (bytes == 1) {
            a_.mov(byte_ptr(rax, offset), kScratch[s].r8());
        } else {
            a_.mov(word_ptr(rax, offset), kScratch[s].r16());
        }
    }
    void store_state_imm(int32_t offset, uint32_t value, int bytes) override {
        if (const Gp* cached = guest(offset)) {
            a_.mov(*cached, imm(value));
            return;
        }
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        if (bytes == 1) {
            a_.mov(byte_ptr(rax, offset), imm(int32_t(value & 0xFF)));
        } else {
            a_.mov(word_ptr(rax, offset), imm(int32_t(value & 0xFFFF)));
        }
    }
    void add_cycles(uint32_t n) override {
        a_.add(kCycles, imm(int32_t(n)));
    }

    void move_imm(int d, uint32_t value) override {
        a_.mov(kScratch[d], imm(value));
    }
    void move(int d, int s) override {
        if (d != s) {
            a_.mov(kScratch[d], kScratch[s]);
        }
    }

    void op(JOp op, int d, int x, int y) override {
        a_.mov(eax, kScratch[x]);
        switch (op) {
            case JOp::Add:
                a_.add(eax, kScratch[y]);
                break;
            case JOp::Sub:
                a_.sub(eax, kScratch[y]);
                break;
            case JOp::And:
                a_.and_(eax, kScratch[y]);
                break;
            case JOp::Or:
                a_.or_(eax, kScratch[y]);
                break;
            case JOp::Xor:
                a_.xor_(eax, kScratch[y]);
                break;
            case JOp::Mul:
                a_.imul(eax, kScratch[y]);
                break;
            case JOp::Shl:
            case JOp::Shr:
                // by register: through cl (rcx is scratch 0: kept in edx around the shift)
                a_.mov(edx, ecx);
                a_.mov(ecx, kScratch[y]);
                if (op == JOp::Shl) {
                    a_.shl(eax, cl);
                } else {
                    a_.shr(eax, cl);
                }
                a_.mov(ecx, edx);
                break;
        }
        a_.mov(kScratch[d], eax);
    }

    void op_imm(JOp op, int d, int x, uint32_t value) override {
        // two-operand forms in place when d == x
        if (d == x && op != JOp::Mul) {
            emit_imm(op, kScratch[d], value);
            return;
        }
        a_.mov(eax, kScratch[x]);
        if (op == JOp::Mul) {
            a_.imul(eax, eax, imm(value));
        } else {
            emit_imm(op, eax, value);
        }
        a_.mov(kScratch[d], eax);
    }

    void set_if(JCond cond, int d, int x, uint32_t value) override {
        a_.cmp(kScratch[x], imm(value));
        switch (cond) {
            case JCond::Eq:
                a_.sete(al);
                break;
            case JCond::Ne:
                a_.setne(al);
                break;
            case JCond::Ltu:
                a_.setb(al);
                break;
            case JCond::Geu:
                a_.setae(al);
                break;
            case JCond::Gtu:
                a_.seta(al);
                break;
            case JCond::Leu:
                a_.setbe(al);
                break;
        }
        a_.movzx(kScratch[d], al);
    }

    void branch_if(JCond cond, int x, uint32_t value, size_t label) override {
        if (value == 0 && (cond == JCond::Eq || cond == JCond::Ne)) {
            a_.test(kScratch[x], kScratch[x]);
        } else {
            a_.cmp(kScratch[x], imm(value));
        }
        const asmjit::Label& l = labels_[label];
        switch (cond) {
            case JCond::Eq:
                a_.je(l);
                break;
            case JCond::Ne:
                a_.jne(l);
                break;
            case JCond::Ltu:
                a_.jb(l);
                break;
            case JCond::Geu:
                a_.jae(l);
                break;
            case JCond::Gtu:
                a_.ja(l);
                break;
            case JCond::Leu:
                a_.jbe(l);
                break;
        }
    }

    void read8(int d, int addr, uint16_t pc) override {
        const asmjit::Label slow = a_.new_label();
        const asmjit::Label done = a_.new_label();
        const Gp address = kScratch[addr].r64();
        a_.mov(rax, qword_ptr(kContext, kCtxRegions));
        a_.cmp(byte_ptr(rax, address), imm(uint32_t(Region::Device)));
        a_.je(slow);
        a_.movzx(kScratch[d], byte_ptr(kMemory, address));
        a_.jmp(done);

        a_.bind(slow);
        store_pc(pc);
        flush_guest();
        spill();
        a_.mov(esi, kScratch[addr]);
        a_.mov(edx, imm(uint32_t(pc)));
        a_.mov(rdi, kContext);
        a_.call(qword_ptr(kContext, kCtxRead));
        a_.mov(edx, eax);
        reload();
        reload_guest();
        a_.mov(kScratch[d], edx);
        a_.bind(done);
    }

    void write8(int addr, int value, uint16_t pc) override {
        const asmjit::Label slow = a_.new_label();
        const asmjit::Label done = a_.new_label();
        const Gp address = kScratch[addr].r64();
        a_.mov(rax, qword_ptr(kContext, kCtxRegions));
        a_.cmp(byte_ptr(rax, address), imm(uint32_t(Region::Ram)));
        a_.jne(slow);
        a_.mov(eax, kScratch[addr]);
        a_.shr(eax, imm(8));
        a_.mov(rdx, qword_ptr(kContext, kCtxCodePages));
        a_.cmp(byte_ptr(rdx, rax), imm(0));
        a_.jne(slow);
        a_.mov(byte_ptr(kMemory, address), kScratch[value].r8());
        a_.jmp(done);

        a_.bind(slow);
        store_pc(pc);
        flush_guest();
        spill();
        a_.mov(esi, kScratch[addr]);
        a_.mov(edx, kScratch[value]);
        a_.mov(ecx, imm(uint32_t(pc)));  // ecx (scratch 0) is spilled: free for the fourth argument
        a_.mov(rdi, kContext);
        a_.call(qword_ptr(kContext, kCtxWrite));
        reload();
        reload_guest();
        a_.bind(done);
    }

    void load_exit(int d) override {
        a_.mov(kScratch[d], dword_ptr(kContext, kCtxExit));
    }
    void load_irq_lines(int d) override {
        a_.mov(rax, qword_ptr(kContext, kCtxIrqLines));
        a_.mov(kScratch[d], dword_ptr(rax));
    }

    void call_step(int d, const Insn* insn, uint32_t remaining) override {
        flush_guest();
        spill();
        a_.mov(rdi, kContext);
        a_.mov(rsi, imm(uint64_t(reinterpret_cast<uintptr_t>(insn))));
        a_.mov(edx, imm(remaining));
        a_.call(qword_ptr(kContext, kCtxStep));
        a_.mov(edx, eax);
        reload();
        reload_guest();
        a_.mov(kScratch[d], edx);
    }

   private:
    void emit_imm(JOp op, const Gp& r, uint32_t value) {
        switch (op) {
            case JOp::Add:
                a_.add(r, imm(value));
                break;
            case JOp::Sub:
                a_.sub(r, imm(value));
                break;
            case JOp::And:
                a_.and_(r, imm(value));
                break;
            case JOp::Or:
                a_.or_(r, imm(value));
                break;
            case JOp::Xor:
                a_.xor_(r, imm(value));
                break;
            case JOp::Shl:
                a_.shl(r, imm(value & 31));
                break;
            case JOp::Shr:
                a_.shr(r, imm(value & 31));
                break;
            case JOp::Mul:
                a_.imul(r, r, imm(value));
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

    // (rax, edx: free at every use)
    void reload_guest() {
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        a_.movzx(kA, byte_ptr(rax, kStateA));
        a_.movzx(kHx, word_ptr(rax, kStateHx));
        a_.movzx(kSp, word_ptr(rax, kStateSp));
        a_.movzx(kCcr, byte_ptr(rax, kStateCcr));
        a_.mov(kCycles, qword_ptr(rax, kStateCycles));
    }
    void flush_guest() {
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        a_.mov(byte_ptr(rax, kStateA), kA.r8());
        a_.mov(word_ptr(rax, kStateHx), kHx.r16());
        a_.mov(word_ptr(rax, kStateSp), kSp.r16());
        a_.mov(byte_ptr(rax, kStateCcr), kCcr.r8());
        a_.mov(qword_ptr(rax, kStateCycles), kCycles);
    }
    void store_pc(uint16_t pc) {
        a_.mov(rax, qword_ptr(kContext, kCtxState));
        a_.mov(word_ptr(rax, kStatePc), imm(int32_t(pc)));
    }

    // the next pc in eax: chain or leave (the scratch registers are free here)
    void dispatch() {
        const asmjit::Label out = a_.new_label();
        const asmjit::Label go = a_.new_label();
        a_.mov(rdx, qword_ptr(kContext, kCtxState));
        a_.mov(word_ptr(rdx, kStatePc), ax);
        a_.cmp(dword_ptr(kContext, kCtxExit), imm(0));
        a_.jne(out);
        a_.mov(rdx, qword_ptr(kContext, kCtxTable));
        a_.mov(rdx, qword_ptr(rdx, rax, 3));
        a_.test(rdx, rdx);
        a_.jz(out);
        a_.mov(rax, qword_ptr(rdx, kChainCycles));
        a_.add(rax, kCycles);
        a_.mov(rcx, qword_ptr(kContext, kCtxLimit));
        a_.cmp(rax, qword_ptr(rcx));
        a_.ja(out);
        a_.mov(rcx, qword_ptr(kContext, kCtxIrqLines));
        a_.cmp(dword_ptr(rcx), imm(0));
        a_.je(go);
        a_.test(kCcr, imm(ccr::kI));
        a_.jz(out);
        a_.bind(go);
        a_.jmp(qword_ptr(rdx, kChainBody));
        a_.bind(out);
        leave();
    }

    void spill() {
        for (int k = 0; k < kScratchRegisters; k++) {
            a_.mov(dword_ptr(kContext, kCtxSpill + 4 * k), kScratch[k]);
        }
    }
    void reload() {
        for (int k = 0; k < kScratchRegisters; k++) {
            a_.mov(kScratch[k], dword_ptr(kContext, kCtxSpill + 4 * k));
        }
    }

    asmjit::CodeHolder* code_;
    Assembler a_;
    asmjit::Label body_;
    std::vector<asmjit::Label> labels_;
};

}  // namespace

std::unique_ptr<BlockAssembler> make_host_assembler(void* code_holder) {
    return std::make_unique<X86Assembler>(static_cast<asmjit::CodeHolder*>(code_holder));
}

}  // namespace sim::jit

#endif
