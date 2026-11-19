// The e200 translator: a block of decoded instructions to host code through the BlockAssembler (e200/jit.h).
//
// Each native form computes exactly what execute_insn does, field for field; e200_engine_diff holds it to the
// interpreter. Anything without a native form is a step-helper call.
#include <asmjit/core.h>

#include <algorithm>
#include <cstring>

#include "emu/jit/jit_error.h"
#include "e200/jit.h"
#include "e200/spe.h"

namespace sim::e200::jit {

namespace {

constexpr int R0 = 0, R1 = 1, R2 = 2, R3 = 3;

// The SPE operations in the one signature generated code calls: (a, b, &spefscr).
namespace spe_jit {
uint32_t cmp_gt(uint32_t a, uint32_t b, uint32_t* f) { return spe::cmp_gt(a, b, f) ? 1 : 0; }
uint32_t cmp_lt(uint32_t a, uint32_t b, uint32_t* f) { return spe::cmp_lt(a, b, f) ? 1 : 0; }
uint32_t cmp_eq(uint32_t a, uint32_t b, uint32_t* f) { return spe::cmp_eq(a, b, f) ? 1 : 0; }
uint32_t tst_gt(uint32_t a, uint32_t b, uint32_t*) { return spe::tst_gt(a, b) ? 1 : 0; }
uint32_t tst_lt(uint32_t a, uint32_t b, uint32_t*) { return spe::tst_lt(a, b) ? 1 : 0; }
uint32_t tst_eq(uint32_t a, uint32_t b, uint32_t*) { return spe::tst_eq(a, b) ? 1 : 0; }
uint32_t cfsi(uint32_t, uint32_t b, uint32_t* f) { return spe::from_signed(int32_t(b), f); }
uint32_t cfui(uint32_t, uint32_t b, uint32_t* f) { return spe::from_unsigned(b, f); }
uint32_t cfsf(uint32_t, uint32_t b, uint32_t* f) { return spe::from_signed_fraction(int32_t(b), f); }
uint32_t cfuf(uint32_t, uint32_t b, uint32_t* f) { return spe::from_unsigned_fraction(b, f); }
uint32_t ctsi(uint32_t, uint32_t b, uint32_t* f) { return spe::to_signed(b, false, f); }
uint32_t ctui(uint32_t, uint32_t b, uint32_t* f) { return spe::to_unsigned(b, false, f); }
uint32_t ctsiz(uint32_t, uint32_t b, uint32_t* f) { return spe::to_signed(b, true, f); }
uint32_t ctuiz(uint32_t, uint32_t b, uint32_t* f) { return spe::to_unsigned(b, true, f); }
uint32_t ctsf(uint32_t, uint32_t b, uint32_t* f) { return spe::to_signed_fraction(b, f); }
uint32_t ctuf(uint32_t, uint32_t b, uint32_t* f) { return spe::to_unsigned_fraction(b, f); }
}  // namespace spe_jit

int32_t gpr(unsigned n) {
    return kStateGpr + int32_t(4 * n);
}

class Translator {
   public:
    Translator(BlockAssembler& a, const Block& block) : a_(a), block_(block) {
        exit_ = a_.new_label();
    }

    // Translates the whole block and binds the shared exit.
    void run() {
        a_.set_block(block_.ea, block_.pa >> 12);
        a_.bind_body();
        const uint32_t n = block_.count();
        bool ended = false;
        for (uint32_t k = 0; k < n; ++k) {
            pc_ = block_.ea + 4 * k;
            k_ = k;
            const Insn& in = block_.insns[k];
            if (native(in)) {
                native_++;
                last_native_ = true;
            } else {
                step(in);
                helpers_++;
                last_native_ = false;
            }
            if (k + 1 == n) {
                ended = ends_with_branch(in);
            }
        }
        if (!ended) {
            // fell off the end (the block's size, its page, or a helper instruction that ends it): go on at the next.
            // After a helper (an MSR write, an interrupt enabled) the engine loop must look before anything runs.
            if (last_native_) {
                a_.chain_imm(block_.ea + 4 * n, n);
            } else {
                a_.store_state_imm(kStatePc, block_.ea + 4 * n);
                a_.set_cycles(n);
                a_.leave();
            }
        }
        // a step helper said to leave: the state it left is the truth
        a_.bind(exit_);
        a_.leave_raw();
    }

    uint32_t native_count() const {
        return native_;
    }
    uint32_t helper_count() const {
        return helpers_;
    }

   private:
    static bool ends_with_branch(const Insn& in) {
        return in.op == Op::B || in.op == Op::Bc || in.op == Op::Bclr || in.op == Op::Bcctr;
    }

    // the instruction through the interpreter; leaves when it says so
    void step(const Insn& in) {
        a_.call_step(R0, &in, pc_, k_);
        a_.branch_if_imm(JCond::Ne, R0, 0, exit_);
    }

    void load_gpr(int d, unsigned n) {
        a_.load_state(d, gpr(n));
    }
    void store_gpr(unsigned n, int s) {
        a_.store_state(gpr(n), s);
    }
    // rA, or 0 for r0
    void load_ra0(int d, const Insn& in) {
        if (in.a == 0) {
            a_.move_imm(d, 0);
        } else {
            load_gpr(d, in.a);
        }
    }

    // CR field `field` = (lt, gt, eq from `cmp` against b) | XER[SO]; `b` is a register or (b < 0) the immediate
    void set_compare_field(unsigned field, int value, int b, uint32_t imm, bool is_signed) {
        a_.compare_field(field, value, b, imm, is_signed);
    }

    // CR0 from a result, when the instruction records
    void record(const Insn& in, int r) {
        if (in.has(flag::kRc)) {
            set_compare_field(0, r, -1, 0, true);
        }
    }

    // rD/rA = r, and CR0 when Rc
    void result_to(unsigned n, const Insn& in, int r) {
        store_gpr(n, r);
        record(in, r);
    }

    static uint32_t rotate_mask(unsigned mb, unsigned me) {
        const uint32_t from_mb = 0xFFFFFFFFu >> mb;
        const uint32_t to_me = 0xFFFFFFFFu << (31 - me);
        return mb <= me ? (from_mb & to_me) : (from_mb | to_me);
    }

    template <int Size, bool Algebraic, bool Indexed, bool Update>
    void load(const Insn& in) {
        if (Update) {
            load_gpr(R0, in.a);
        } else {
            load_ra0(R0, in);
        }
        if (Indexed) {
            load_gpr(R1, in.b);
            a_.op(JOp::Add, R0, R0, R1);
        } else if (in.imm != 0) {
            a_.op_imm(JOp::Add, R0, R0, uint32_t(in.imm));
        }
        const size_t slow = a_.new_label();
        const size_t done = a_.new_label();
        a_.mem_load(R1, R0, Size, slow);
        if (Algebraic) {
            a_.op_imm(JOp::Shl, R1, R1, 16);
            a_.op_imm(JOp::Sar, R1, R1, 16);
        }
        if (Update) {
            store_gpr(in.a, R0);
        }
        store_gpr(in.d, R1);
        a_.jump(done);
        a_.bind(slow);
        step(in);
        a_.bind(done);
    }

    template <int Size, bool Indexed, bool Update>
    void store(const Insn& in) {
        if (Update) {
            load_gpr(R0, in.a);
        } else {
            load_ra0(R0, in);
        }
        if (Indexed) {
            load_gpr(R1, in.b);
            a_.op(JOp::Add, R0, R0, R1);
        } else if (in.imm != 0) {
            a_.op_imm(JOp::Add, R0, R0, uint32_t(in.imm));
        }
        load_gpr(R1, in.d);
        const size_t slow = a_.new_label();
        const size_t done = a_.new_label();
        a_.mem_store(R0, R1, Size, slow);
        if (Update) {
            store_gpr(in.a, R0);
        }
        a_.jump(done);
        a_.bind(slow);
        step(in);
        a_.bind(done);
    }

    // logical X-form: rA = rS op rB
    void logical(const Insn& in, JOp op, bool complement_b, bool complement_result) {
        load_gpr(R0, in.d);
        load_gpr(R1, in.b);
        if (complement_b) {
            a_.not_(R1, R1);
        }
        a_.op(op, R0, R0, R1);
        if (complement_result) {
            a_.not_(R0, R0);
        }
        result_to(in.a, in, R0);
    }

    // the conditional branches: bc, bclr, bcctr (they end the block)
    void branch_conditional(const Insn& in) {
        const unsigned bo = in.d;
        if (in.op == Op::Bclr) {
            a_.load_state(R3, kStateLr);
            a_.op_imm(JOp::And, R3, R3, ~3u);
        } else if (in.op == Op::Bcctr) {
            a_.load_state(R3, kStateCtr);
            a_.op_imm(JOp::And, R3, R3, ~3u);
        }
        const bool decrements = !(bo & 0x04) && in.op != Op::Bcctr;
        if (decrements) {
            a_.load_state(R0, kStateCtr);
            a_.op_imm(JOp::Sub, R0, R0, 1);
            a_.store_state(kStateCtr, R0);
        }
        if (in.has(flag::kLk)) {
            a_.store_state_imm(kStateLr, pc_ + 4);
        }
        const size_t not_taken = a_.new_label();
        if (decrements) {
            a_.branch_if_imm((bo & 0x02) ? JCond::Ne : JCond::Eq, R0, 0, not_taken);
        }
        if (!(bo & 0x10)) {
            a_.load_state(R1, kStateCr);
            a_.op_imm(JOp::Shr, R1, R1, 31 - in.a);
            a_.op_imm(JOp::And, R1, R1, 1);
            a_.branch_if_imm((bo & 0x08) ? JCond::Eq : JCond::Ne, R1, 0, not_taken);
        }
        if (in.op == Op::Bc) {
            a_.chain_imm(in.has(flag::kAa) ? uint32_t(in.imm) : pc_ + uint32_t(in.imm), k_ + 1);
        } else {
            a_.chain_reg(R3, k_ + 1);
        }
        a_.bind(not_taken);
        a_.chain_imm(pc_ + 4, k_ + 1);
    }

    // rD = fn(rA, rB)
    void spe_binary(const Insn& in, BlockAssembler::SpeFunction fn) {
        load_gpr(R0, in.a);
        load_gpr(R1, in.b);
        a_.call_spe(R0, fn, R0, R1);
        store_gpr(in.d, R0);
    }
    // rD = fn(rB)
    void spe_unary(const Insn& in, BlockAssembler::SpeFunction fn) {
        load_gpr(R0, in.b);
        a_.call_spe(R0, fn, R0, R0);
        store_gpr(in.d, R0);
    }
    // CR field crfD = fn(rA, rB) ? GT : 0
    void spe_compare(const Insn& in, BlockAssembler::SpeFunction fn) {
        load_gpr(R0, in.a);
        load_gpr(R1, in.b);
        a_.call_spe(R0, fn, R0, R1);
        const uint32_t shift = 28 - 4 * (in.d >> 2);
        a_.op_imm(JOp::Shl, R0, R0, shift + 2);  // the GT bit of the field
        a_.load_state(R1, kStateCr);
        a_.op_imm(JOp::And, R1, R1, ~(0xFu << shift));
        a_.op(JOp::Or, R1, R1, R0);
        a_.store_state(kStateCr, R1);
    }

    // The native forms; false for an instruction the step helper runs.
    bool native(const Insn& in) {
        const bool oe = in.has(flag::kOe);
        switch (in.op) {
            case Op::Addi:
            case Op::Addis: {
                const uint32_t imm = in.op == Op::Addis ? uint32_t(in.imm) << 16 : uint32_t(in.imm);
                if (in.a == 0) {
                    a_.move_imm(R0, imm);
                } else {
                    load_gpr(R0, in.a);
                    a_.op_imm(JOp::Add, R0, R0, imm);
                }
                store_gpr(in.d, R0);
                return true;
            }
            case Op::Mulli:
                load_gpr(R0, in.a);
                a_.move_imm(R1, uint32_t(in.imm));
                a_.op(JOp::Mul, R0, R0, R1);
                store_gpr(in.d, R0);
                return true;
            case Op::Add:
            case Op::Subf:
            case Op::Mullw:
                if (oe) {
                    return false;
                }
                load_gpr(R0, in.a);
                load_gpr(R1, in.b);
                if (in.op == Op::Add) {
                    a_.op(JOp::Add, R0, R0, R1);
                } else if (in.op == Op::Subf) {
                    a_.op(JOp::Sub, R0, R1, R0);
                } else {
                    a_.op(JOp::Mul, R0, R0, R1);
                }
                result_to(in.d, in, R0);
                return true;
            case Op::Neg:
                if (oe) {
                    return false;
                }
                load_gpr(R1, in.a);
                a_.move_imm(R0, 0);
                a_.op(JOp::Sub, R0, R0, R1);
                result_to(in.d, in, R0);
                return true;

            case Op::AndiRc:
            case Op::AndisRc: {
                const uint32_t imm = in.op == Op::AndisRc ? uint32_t(in.imm) << 16 : uint32_t(in.imm);
                load_gpr(R0, in.d);
                a_.op_imm(JOp::And, R0, R0, imm);
                store_gpr(in.a, R0);
                set_compare_field(0, R0, -1, 0, true);
                return true;
            }
            case Op::Ori:
            case Op::Oris:
            case Op::Xori:
            case Op::Xoris: {
                const bool shifted = in.op == Op::Oris || in.op == Op::Xoris;
                const uint32_t imm = shifted ? uint32_t(in.imm) << 16 : uint32_t(in.imm);
                load_gpr(R0, in.d);
                a_.op_imm((in.op == Op::Ori || in.op == Op::Oris) ? JOp::Or : JOp::Xor, R0, R0, imm);
                store_gpr(in.a, R0);
                return true;
            }
            case Op::And: logical(in, JOp::And, false, false); return true;
            case Op::Andc: logical(in, JOp::And, true, false); return true;
            case Op::Or: logical(in, JOp::Or, false, false); return true;
            case Op::Orc: logical(in, JOp::Or, true, false); return true;
            case Op::Xor: logical(in, JOp::Xor, false, false); return true;
            case Op::Nand: logical(in, JOp::And, false, true); return true;
            case Op::Nor: logical(in, JOp::Or, false, true); return true;
            case Op::Eqv: logical(in, JOp::Xor, false, true); return true;
            case Op::Extsb:
            case Op::Extsh: {
                const uint32_t shift = in.op == Op::Extsb ? 24 : 16;
                load_gpr(R0, in.d);
                a_.op_imm(JOp::Shl, R0, R0, shift);
                a_.op_imm(JOp::Sar, R0, R0, shift);
                result_to(in.a, in, R0);
                return true;
            }
            case Op::Cntlzw:
                load_gpr(R0, in.d);
                a_.count_leading_zeros(R0, R0);
                result_to(in.a, in, R0);
                return true;

            case Op::Rlwinm:
                load_gpr(R0, in.d);
                if (in.b & 31) {
                    a_.op_imm(JOp::Rotl, R0, R0, in.b & 31u);
                }
                a_.op_imm(JOp::And, R0, R0, rotate_mask(in.c, in.e));
                result_to(in.a, in, R0);
                return true;
            case Op::Rlwnm:
                load_gpr(R0, in.d);
                load_gpr(R1, in.b);
                a_.op_imm(JOp::And, R1, R1, 31);
                a_.op(JOp::Rotl, R0, R0, R1);
                a_.op_imm(JOp::And, R0, R0, rotate_mask(in.c, in.e));
                result_to(in.a, in, R0);
                return true;
            case Op::Rlwimi: {
                const uint32_t m = rotate_mask(in.c, in.e);
                load_gpr(R0, in.d);
                if (in.b & 31) {
                    a_.op_imm(JOp::Rotl, R0, R0, in.b & 31u);
                }
                a_.op_imm(JOp::And, R0, R0, m);
                load_gpr(R1, in.a);
                a_.op_imm(JOp::And, R1, R1, ~m);
                a_.op(JOp::Or, R0, R0, R1);
                result_to(in.a, in, R0);
                return true;
            }
            case Op::Slw:
            case Op::Srw: {
                const size_t zero = a_.new_label();
                const size_t done = a_.new_label();
                load_gpr(R0, in.d);
                load_gpr(R1, in.b);
                a_.op_imm(JOp::And, R1, R1, 63);
                a_.branch_if_imm(JCond::Gtu, R1, 31, zero);
                a_.op(in.op == Op::Slw ? JOp::Shl : JOp::Shr, R0, R0, R1);
                a_.jump(done);
                a_.bind(zero);
                a_.move_imm(R0, 0);
                a_.bind(done);
                result_to(in.a, in, R0);
                return true;
            }

            case Op::Cmp:
            case Op::Cmpl:
                load_gpr(R0, in.a);
                load_gpr(R1, in.b);
                set_compare_field(in.d >> 2, R0, R1, 0, in.op == Op::Cmp);
                return true;
            case Op::Cmpi:
            case Op::Cmpli:
                load_gpr(R0, in.a);
                set_compare_field(in.d >> 2, R0, -1, uint32_t(in.imm), in.op == Op::Cmpi);
                return true;

            case Op::Mfcr:
                a_.load_state(R0, kStateCr);
                store_gpr(in.d, R0);
                return true;
            case Op::Mtcrf: {
                uint32_t mask = 0;
                for (unsigned f = 0; f < 8; ++f) {
                    if (uint32_t(in.imm) & (0x80u >> f)) {
                        mask |= 0xF0000000u >> (4 * f);
                    }
                }
                load_gpr(R0, in.d);
                a_.op_imm(JOp::And, R0, R0, mask);
                a_.load_state(R1, kStateCr);
                a_.op_imm(JOp::And, R1, R1, ~mask);
                a_.op(JOp::Or, R0, R0, R1);
                a_.store_state(kStateCr, R0);
                return true;
            }
            case Op::Isel: {
                const size_t keep = a_.new_label();
                a_.load_state(R2, kStateCr);
                a_.op_imm(JOp::Shr, R2, R2, 31 - in.c);
                a_.op_imm(JOp::And, R2, R2, 1);
                load_ra0(R0, in);
                a_.branch_if_imm(JCond::Ne, R2, 0, keep);
                load_gpr(R0, in.b);
                a_.bind(keep);
                store_gpr(in.d, R0);
                return true;
            }
            case Op::Mfspr: {
                int32_t field = -1;
                if (in.spr == spr::kLr) {
                    field = kStateLr;
                } else if (in.spr == spr::kCtr) {
                    field = kStateCtr;
                } else if (in.spr == spr::kXer) {
                    field = kStateXer;
                }
                if (field < 0) {
                    return false;
                }
                a_.load_state(R0, field);
                store_gpr(in.d, R0);
                return true;
            }
            case Op::Mtspr: {
                if (in.spr != spr::kLr && in.spr != spr::kCtr && in.spr != spr::kXer) {
                    return false;
                }
                load_gpr(R0, in.d);
                if (in.spr == spr::kXer) {
                    a_.op_imm(JOp::And, R0, R0, 0xE000007F);
                }
                a_.store_state(in.spr == spr::kLr ? kStateLr : in.spr == spr::kCtr ? kStateCtr : kStateXer, R0);
                return true;
            }

            case Op::Lbz: load<1, false, false, false>(in); return true;
            case Op::Lbzu: load<1, false, false, true>(in); return true;
            case Op::Lbzx: load<1, false, true, false>(in); return true;
            case Op::Lbzux: load<1, false, true, true>(in); return true;
            case Op::Lhz: load<2, false, false, false>(in); return true;
            case Op::Lhzu: load<2, false, false, true>(in); return true;
            case Op::Lhzx: load<2, false, true, false>(in); return true;
            case Op::Lhzux: load<2, false, true, true>(in); return true;
            case Op::Lha: load<2, true, false, false>(in); return true;
            case Op::Lhau: load<2, true, false, true>(in); return true;
            case Op::Lhax: load<2, true, true, false>(in); return true;
            case Op::Lhaux: load<2, true, true, true>(in); return true;
            case Op::Lwz: load<4, false, false, false>(in); return true;
            case Op::Lwzu: load<4, false, false, true>(in); return true;
            case Op::Lwzx: load<4, false, true, false>(in); return true;
            case Op::Lwzux: load<4, false, true, true>(in); return true;
            case Op::Stb: store<1, false, false>(in); return true;
            case Op::Stbu: store<1, false, true>(in); return true;
            case Op::Stbx: store<1, true, false>(in); return true;
            case Op::Stbux: store<1, true, true>(in); return true;
            case Op::Sth: store<2, false, false>(in); return true;
            case Op::Sthu: store<2, false, true>(in); return true;
            case Op::Sthx: store<2, true, false>(in); return true;
            case Op::Sthux: store<2, true, true>(in); return true;
            case Op::Stw: store<4, false, false>(in); return true;
            case Op::Stwu: store<4, false, true>(in); return true;
            case Op::Stwx: store<4, true, false>(in); return true;
            case Op::Stwux: store<4, true, true>(in); return true;

            case Op::B:
                if (in.has(flag::kLk)) {
                    a_.store_state_imm(kStateLr, pc_ + 4);
                }
                a_.chain_imm(in.has(flag::kAa) ? uint32_t(in.imm) : pc_ + uint32_t(in.imm), k_ + 1);
                return true;
            case Op::Bc:
            case Op::Bclr:
            case Op::Bcctr:
                branch_conditional(in);
                return true;

            case Op::CacheNop:
            case Op::Msync:
            case Op::Mbar:
                return true;

            // SPE: the arithmetic by direct call (it cannot fault or branch), the sign operations inline
            case Op::Efsadd: spe_binary(in, &spe::add); return true;
            case Op::Efssub: spe_binary(in, &spe::sub); return true;
            case Op::Efsmul: spe_binary(in, &spe::mul); return true;
            case Op::Efsdiv: spe_binary(in, &spe::div); return true;
            case Op::Efsabs:
            case Op::Efsnabs:
            case Op::Efsneg:
                load_gpr(R0, in.a);
                if (in.op == Op::Efsabs) {
                    a_.op_imm(JOp::And, R0, R0, 0x7FFFFFFF);
                } else {
                    a_.op_imm(in.op == Op::Efsnabs ? JOp::Or : JOp::Xor, R0, R0, 0x80000000);
                }
                store_gpr(in.d, R0);
                return true;
            case Op::Efscmpgt: spe_compare(in, &spe_jit::cmp_gt); return true;
            case Op::Efscmplt: spe_compare(in, &spe_jit::cmp_lt); return true;
            case Op::Efscmpeq: spe_compare(in, &spe_jit::cmp_eq); return true;
            case Op::Efststgt: spe_compare(in, &spe_jit::tst_gt); return true;
            case Op::Efststlt: spe_compare(in, &spe_jit::tst_lt); return true;
            case Op::Efststeq: spe_compare(in, &spe_jit::tst_eq); return true;
            case Op::Efscfsi: spe_unary(in, &spe_jit::cfsi); return true;
            case Op::Efscfui: spe_unary(in, &spe_jit::cfui); return true;
            case Op::Efscfsf: spe_unary(in, &spe_jit::cfsf); return true;
            case Op::Efscfuf: spe_unary(in, &spe_jit::cfuf); return true;
            case Op::Efsctsi: spe_unary(in, &spe_jit::ctsi); return true;
            case Op::Efsctui: spe_unary(in, &spe_jit::ctui); return true;
            case Op::Efsctsiz: spe_unary(in, &spe_jit::ctsiz); return true;
            case Op::Efsctuiz: spe_unary(in, &spe_jit::ctuiz); return true;
            case Op::Efsctsf: spe_unary(in, &spe_jit::ctsf); return true;
            case Op::Efsctuf: spe_unary(in, &spe_jit::ctuf); return true;

            // run by the step helper
            case Op::Illegal:
            case Op::Addic:
            case Op::AddicRc:
            case Op::Subfic:
            case Op::Addc:
            case Op::Adde:
            case Op::Addme:
            case Op::Addze:
            case Op::Subfc:
            case Op::Subfe:
            case Op::Subfme:
            case Op::Subfze:
            case Op::Mulhw:
            case Op::Mulhwu:
            case Op::Divw:
            case Op::Divwu:
            case Op::Sraw:
            case Op::Srawi:
            case Op::Mcrf:
            case Op::Crand:
            case Op::Crandc:
            case Op::Creqv:
            case Op::Crnand:
            case Op::Crnor:
            case Op::Cror:
            case Op::Crorc:
            case Op::Crxor:
            case Op::Mcrxr:
            case Op::Lhbrx:
            case Op::Lwbrx:
            case Op::Sthbrx:
            case Op::Stwbrx:
            case Op::Lmw:
            case Op::Stmw:
            case Op::Lwarx:
            case Op::StwcxRc:
            case Op::Sc:
            case Op::Tw:
            case Op::Twi:
            case Op::Mfmsr:
            case Op::Mtmsr:
            case Op::Wrtee:
            case Op::Wrteei:
            case Op::Rfi:
            case Op::Rfci:
            case Op::Rfdi:
            case Op::Rfmci:
            case Op::Wait:
            case Op::Isync:
            case Op::Dcbz:
            case Op::Tlbwe:
            case Op::Tlbre:
            case Op::Tlbsx:
            case Op::Tlbivax:
            case Op::Tlbsync:
            case Op::Count_:
                return false;
        }
        return false;
    }

    BlockAssembler& a_;
    const Block& block_;
    size_t exit_ = 0;
    uint32_t pc_ = 0;
    uint32_t k_ = 0;
    uint32_t native_ = 0;
    uint32_t helpers_ = 0;
    bool last_native_ = false;
};

}  // namespace

// The CpuState fields a block uses most, for the assembler to keep in host registers: each instruction's register
// fields (an over-count for the forms whose fields are not all registers), CR for compares and conditional branches,
// CTR and LR for the branches and moves that use them. Only fields used at least twice are worth a register.
std::vector<int32_t> choose_cached(const std::vector<Insn>& insns) {
    std::vector<std::pair<int32_t, int>> uses;
    auto use = [&uses](int32_t offset) {
        for (auto& u : uses) {
            if (u.first == offset) {
                u.second++;
                return;
            }
        }
        uses.push_back({offset, 1});
    };
    for (const Insn& in : insns) {
        const Op op = in.op;
        if (op == Op::B) {
            if (in.has(flag::kLk)) {
                use(kStateLr);
            }
            continue;
        }
        if (op == Op::Bc || op == Op::Bclr || op == Op::Bcctr) {
            if (!(in.d & 0x10)) {
                use(kStateCr);
            }
            if (!(in.d & 0x04) || op == Op::Bcctr) {
                use(kStateCtr);
            }
            if (op == Op::Bclr || in.has(flag::kLk)) {
                use(kStateLr);
            }
            continue;
        }
        if (op == Op::Cmp || op == Op::Cmpi || op == Op::Cmpl || op == Op::Cmpli || op == Op::Isel ||
            op == Op::Mfcr || op == Op::Mtcrf || in.has(flag::kRc)) {
            use(kStateCr);
        }
        if ((op == Op::Mfspr || op == Op::Mtspr) && in.spr == spr::kLr) {
            use(kStateLr);
        }
        if ((op == Op::Mfspr || op == Op::Mtspr) && in.spr == spr::kCtr) {
            use(kStateCtr);
        }
        use(int32_t(kStateGpr + 4 * int32_t(in.d)));
        use(int32_t(kStateGpr + 4 * int32_t(in.a)));
        use(int32_t(kStateGpr + 4 * int32_t(in.b)));
    }
    std::stable_sort(uses.begin(), uses.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::vector<int32_t> out;
    for (const auto& u : uses) {
        if (u.second >= 2 && out.size() < size_t(kCachedRegisters)) {
            out.push_back(u.first);
        }
    }
    return out;
}

std::unique_ptr<Block> translate(sim::jit::CodeMemory& memory, uint32_t ea, uint32_t pa, std::vector<Insn> insns) {
    auto block = std::make_unique<Block>();
    block->ea = ea;
    block->pa = pa;
    block->insns = std::move(insns);

    asmjit::CodeHolder code;
    code.init(asmjit::Environment::host());
    std::unique_ptr<BlockAssembler> a = make_host_assembler(&code);
    if (!a) {
        throw sim::jit::JitError("no e200 JIT assembler for this host");
    }
    a->set_cached(choose_cached(block->insns));
    a->prologue();
    Translator t(*a, *block);
    t.run();
    block->native = t.native_count();
    block->helpers = t.helper_count();

    if (code.flatten() != asmjit::Error::kOk) {
        throw sim::jit::JitError("asmjit flatten failed");
    }
    // each block starts on a 64-byte boundary, so the alignment inside it (the body, a loop's head) is real
    constexpr size_t kAlign = 64;
    const size_t size = code.code_size();
    sim::jit::CodeMemory::WritableScope scope = memory.make_writable();
    auto span = scope.allocate(size + kAlign);
    if (!span) {
        return nullptr;
    }
    std::byte* runs_from = memory.executable_address(span->data());
    const size_t skip = (kAlign - (reinterpret_cast<uintptr_t>(runs_from) & (kAlign - 1))) & (kAlign - 1);
    std::byte* runs_at = runs_from + skip;
    if (code.relocate_to_base(uint64_t(reinterpret_cast<uintptr_t>(runs_at))) != asmjit::Error::kOk ||
        code.copy_flattened_data(span->data() + skip, size) != asmjit::Error::kOk) {
        throw sim::jit::JitError("asmjit relocate/copy failed");
    }
    std::memcpy(&block->entry, &runs_at, sizeof(runs_at));
    block->chain.ea = ea;
    block->chain.pa_page = pa >> 12;
    block->chain.count = block->count();
    block->chain.body = runs_at + a->body_offset();
    block->chain.block = block.get();
    return block;
}

// ------------------------------------------------------------------------------------------------ the cache

Block* JitCache::insert(uint32_t ea, uint32_t pa, std::vector<Insn> insns) {
    std::unique_ptr<Block> block = translate(memory_, ea, pa, insns);
    if (!block) {
        clear();
        collect();
        memory_.reset();
        stats.resets++;
        block = translate(memory_, ea, pa, std::move(insns));
        if (!block) {
            return nullptr;
        }
    }
    stats.translated++;
    Block* raw = block.get();
    auto it = blocks_.find(ea);
    if (it != blocks_.end()) {
        unlink(*it->second);
        dropped_.push_back(std::move(it->second));
        stats.dropped++;
    }
    blocks_[ea] = std::move(block);
    by_page_[pa >> 12].push_back(ea);
    table_[(ea >> 2) & (kChainTableSize - 1)] = &raw->chain;
    return raw;
}

void JitCache::unlink(const Block& block) {
    ChainEntry*& slot = table_[(block.ea >> 2) & (kChainTableSize - 1)];
    if (slot == &block.chain) {
        slot = nullptr;
    }
}

void JitCache::drop_page(uint32_t page) {
    auto it = by_page_.find(page);
    if (it == by_page_.end()) {
        return;
    }
    for (uint32_t ea : it->second) {
        auto b = blocks_.find(ea);
        if (b != blocks_.end() && (b->second->pa >> 12) == page) {
            unlink(*b->second);
            dropped_.push_back(std::move(b->second));
            blocks_.erase(b);
            stats.dropped++;
        }
    }
    by_page_.erase(it);
}

void JitCache::clear() {
    for (auto& [ea, block] : blocks_) {
        dropped_.push_back(std::move(block));
        stats.dropped++;
    }
    blocks_.clear();
    by_page_.clear();
    std::fill(table_.begin(), table_.end(), nullptr);
}

}  // namespace sim::e200::jit
