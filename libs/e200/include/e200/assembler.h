// A small e200 assembler for programs written in C++: the core's tests and the bare images the SoC's tests boot, so
// that no target toolchain is needed in the tree.
//
// Each method appends one instruction word (big-endian order is applied by bytes()). Branch targets are labels, bound
// anywhere before words()/bytes(); the extended mnemonics the tests use (li, lis, mr, blr, beq, ...) are spelled out.
//
//   Asm a(0x1000);
//   auto loop = a.label();
//   a.li(3, 10);
//   a.bind(loop);
//   a.addic_(3, 3, -1);
//   a.bne(loop);
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sim::e200 {

class Asm {
   public:
    using Label = size_t;

    explicit Asm(uint32_t origin = 0) : origin_(origin) {
    }

    uint32_t origin() const {
        return origin_;
    }
    // the address of the next instruction
    uint32_t here() const {
        return origin_ + uint32_t(words_.size() * 4);
    }

    Label label() {
        labels_.push_back(kUnbound);
        return labels_.size() - 1;
    }
    void bind(Label l) {
        labels_.at(l) = here();
    }
    uint32_t address(Label l) const {
        return labels_.at(l);
    }

    void word(uint32_t w) {
        words_.push_back(w);
    }

    // ---- D-form
    void addi(int d, int a, int16_t imm) { d_form(14, d, a, uint16_t(imm)); }
    void addis(int d, int a, int16_t imm) { d_form(15, d, a, uint16_t(imm)); }
    void addic(int d, int a, int16_t imm) { d_form(12, d, a, uint16_t(imm)); }
    void addic_(int d, int a, int16_t imm) { d_form(13, d, a, uint16_t(imm)); }
    void subfic(int d, int a, int16_t imm) { d_form(8, d, a, uint16_t(imm)); }
    void mulli(int d, int a, int16_t imm) { d_form(7, d, a, uint16_t(imm)); }
    void ori(int a, int s, uint16_t imm) { d_form(24, s, a, imm); }
    void oris(int a, int s, uint16_t imm) { d_form(25, s, a, imm); }
    void xori(int a, int s, uint16_t imm) { d_form(26, s, a, imm); }
    void andi_(int a, int s, uint16_t imm) { d_form(28, s, a, imm); }
    void andis_(int a, int s, uint16_t imm) { d_form(29, s, a, imm); }
    void cmpwi(int crf, int a, int16_t imm) { d_form(11, crf << 2, a, uint16_t(imm)); }
    void cmplwi(int crf, int a, uint16_t imm) { d_form(10, crf << 2, a, imm); }
    void twi(int to, int a, int16_t imm) { d_form(3, to, a, uint16_t(imm)); }
    void lwz(int d, int16_t off, int a) { d_form(32, d, a, uint16_t(off)); }
    void lwzu(int d, int16_t off, int a) { d_form(33, d, a, uint16_t(off)); }
    void lbz(int d, int16_t off, int a) { d_form(34, d, a, uint16_t(off)); }
    void lhz(int d, int16_t off, int a) { d_form(40, d, a, uint16_t(off)); }
    void lha(int d, int16_t off, int a) { d_form(42, d, a, uint16_t(off)); }
    void stw(int s, int16_t off, int a) { d_form(36, s, a, uint16_t(off)); }
    void stwu(int s, int16_t off, int a) { d_form(37, s, a, uint16_t(off)); }
    void stb(int s, int16_t off, int a) { d_form(38, s, a, uint16_t(off)); }
    void sth(int s, int16_t off, int a) { d_form(44, s, a, uint16_t(off)); }
    void lmw(int d, int16_t off, int a) { d_form(46, d, a, uint16_t(off)); }
    void stmw(int s, int16_t off, int a) { d_form(47, s, a, uint16_t(off)); }

    // ---- extended mnemonics
    void li(int d, int16_t imm) { addi(d, 0, imm); }
    void lis(int d, int16_t imm) { addis(d, 0, imm); }
    // d = any 32-bit constant (lis; ori)
    void li32(int d, uint32_t v) {
        lis(d, int16_t(uint16_t(v >> 16)));
        ori(d, d, uint16_t(v));
    }
    void mr(int a, int s) { or_(a, s, s); }
    void nop() { ori(0, 0, 0); }

    // ---- X/XO-form (rc: the record form; oe: the overflow form)
    void add(int d, int a, int b, bool rc = false, bool oe = false) { xo(266, d, a, b, rc, oe); }
    void addc(int d, int a, int b, bool rc = false, bool oe = false) { xo(10, d, a, b, rc, oe); }
    void adde(int d, int a, int b, bool rc = false, bool oe = false) { xo(138, d, a, b, rc, oe); }
    void addze(int d, int a, bool rc = false) { xo(202, d, a, 0, rc, false); }
    void addme(int d, int a, bool rc = false) { xo(234, d, a, 0, rc, false); }
    void subf(int d, int a, int b, bool rc = false, bool oe = false) { xo(40, d, a, b, rc, oe); }
    void subfc(int d, int a, int b, bool rc = false, bool oe = false) { xo(8, d, a, b, rc, oe); }
    void subfe(int d, int a, int b, bool rc = false, bool oe = false) { xo(136, d, a, b, rc, oe); }
    void subfze(int d, int a, bool rc = false) { xo(200, d, a, 0, rc, false); }
    void neg(int d, int a, bool rc = false, bool oe = false) { xo(104, d, a, 0, rc, oe); }
    void mullw(int d, int a, int b, bool rc = false, bool oe = false) { xo(235, d, a, b, rc, oe); }
    void mulhw(int d, int a, int b) { xo(75, d, a, b, false, false); }
    void mulhwu(int d, int a, int b) { xo(11, d, a, b, false, false); }
    void divw(int d, int a, int b, bool rc = false, bool oe = false) { xo(491, d, a, b, rc, oe); }
    void divwu(int d, int a, int b, bool rc = false, bool oe = false) { xo(459, d, a, b, rc, oe); }
    void and_(int a, int s, int b, bool rc = false) { x(28, s, a, b, rc); }
    void andc(int a, int s, int b, bool rc = false) { x(60, s, a, b, rc); }
    void or_(int a, int s, int b, bool rc = false) { x(444, s, a, b, rc); }
    void orc(int a, int s, int b, bool rc = false) { x(412, s, a, b, rc); }
    void xor_(int a, int s, int b, bool rc = false) { x(316, s, a, b, rc); }
    void nand(int a, int s, int b, bool rc = false) { x(476, s, a, b, rc); }
    void nor(int a, int s, int b, bool rc = false) { x(124, s, a, b, rc); }
    void eqv(int a, int s, int b, bool rc = false) { x(284, s, a, b, rc); }
    void extsb(int a, int s, bool rc = false) { x(954, s, a, 0, rc); }
    void extsh(int a, int s, bool rc = false) { x(922, s, a, 0, rc); }
    void cntlzw(int a, int s, bool rc = false) { x(26, s, a, 0, rc); }
    void slw(int a, int s, int b, bool rc = false) { x(24, s, a, b, rc); }
    void srw(int a, int s, int b, bool rc = false) { x(536, s, a, b, rc); }
    void sraw(int a, int s, int b, bool rc = false) { x(792, s, a, b, rc); }
    void srawi(int a, int s, int sh, bool rc = false) { x(824, s, a, sh, rc); }
    void rlwinm(int a, int s, int sh, int mb, int me, bool rc = false) { m_form(21, s, a, sh, mb, me, rc); }
    void rlwnm(int a, int s, int b, int mb, int me, bool rc = false) { m_form(23, s, a, b, mb, me, rc); }
    void rlwimi(int a, int s, int sh, int mb, int me, bool rc = false) { m_form(20, s, a, sh, mb, me, rc); }
    void slwi(int a, int s, int n) { rlwinm(a, s, n, 0, 31 - n); }
    void srwi(int a, int s, int n) { rlwinm(a, s, 32 - n, n, 31); }
    void cmpw(int crf, int a, int b) { x(0, crf << 2, a, b, false); }
    void cmplw(int crf, int a, int b) { x(32, crf << 2, a, b, false); }
    void lwzx(int d, int a, int b) { x(23, d, a, b, false); }
    void lbzx(int d, int a, int b) { x(87, d, a, b, false); }
    void lhzx(int d, int a, int b) { x(279, d, a, b, false); }
    void stwx(int s, int a, int b) { x(151, s, a, b, false); }
    void stbx(int s, int a, int b) { x(215, s, a, b, false); }
    void sthx(int s, int a, int b) { x(407, s, a, b, false); }
    void lwbrx(int d, int a, int b) { x(534, d, a, b, false); }
    void stwbrx(int s, int a, int b) { x(662, s, a, b, false); }
    void lhbrx(int d, int a, int b) { x(790, d, a, b, false); }
    void lwarx(int d, int a, int b) { x(20, d, a, b, false); }
    void stwcx_(int s, int a, int b) { x(150, s, a, b, true); }
    void dcbz(int a, int b) { x(1014, 0, a, b, false); }
    void tw(int to, int a, int b) { x(4, to, a, b, false); }
    void mfcr(int d) { x(19, d, 0, 0, false); }
    void mtcrf(int crm, int s) { word((31u << 26) | (uint32_t(s) << 21) | (uint32_t(crm) << 12) | (144u << 1)); }
    void mfmsr(int d) { x(83, d, 0, 0, false); }
    void mtmsr(int s) { x(146, s, 0, 0, false); }
    void wrtee(int s) { x(131, s, 0, 0, false); }
    void wrteei(int e) { word((31u << 26) | (uint32_t(e & 1) << 15) | (163u << 1)); }
    void mfspr(int d, int spr) { word((31u << 26) | (uint32_t(d) << 21) | spr_field(spr) | (339u << 1)); }
    void mtspr(int spr, int s) { word((31u << 26) | (uint32_t(s) << 21) | spr_field(spr) | (467u << 1)); }
    void mflr(int d) { mfspr(d, 8); }
    void mtlr(int s) { mtspr(8, s); }
    void mfctr(int d) { mfspr(d, 9); }
    void mtctr(int s) { mtspr(9, s); }
    void isel(int d, int a, int b, int crb) {
        word((31u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (uint32_t(crb) << 6) |
             (15u << 1));
    }
    void tlbwe() { x(978, 0, 0, 0, false); }
    void tlbre() { x(946, 0, 0, 0, false); }
    void tlbsx(int a, int b) { x(914, 0, a, b, false); }
    void tlbivax(int a, int b) { x(786, 0, a, b, false); }
    void msync() { x(598, 0, 0, 0, false); }
    void wait() { x(62, 0, 0, 0, false); }

    // ---- XL-form
    void blr() { word(0x4E800020); }
    void blrl() { word(0x4E800021); }
    void bctr() { word(0x4E800420); }
    void bctrl() { word(0x4E800421); }
    void rfi() { word(0x4C000064); }
    void rfci() { word(0x4C000066); }
    void isync() { word(0x4C00012C); }
    void sc() { word(0x44000002); }
    void crxor(int d, int a, int b) { word((19u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (193u << 1)); }
    void cror(int d, int a, int b) { word((19u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (449u << 1)); }

    // ---- branches to labels
    void b(Label l) { branch(l, false); }
    void bl(Label l) { branch(l, true); }
    // bc BO,BI,label
    void bc(int bo, int bi, Label l, bool lk = false) {
        fixups_.push_back({words_.size(), l, true});
        word((16u << 26) | (uint32_t(bo) << 21) | (uint32_t(bi) << 16) | (lk ? 1u : 0u));
    }
    // on CR0 (crf selects another field)
    void beq(Label l, int crf = 0) { bc(12, crf * 4 + 2, l); }
    void bne(Label l, int crf = 0) { bc(4, crf * 4 + 2, l); }
    void blt(Label l, int crf = 0) { bc(12, crf * 4 + 0, l); }
    void bge(Label l, int crf = 0) { bc(4, crf * 4 + 0, l); }
    void bgt(Label l, int crf = 0) { bc(12, crf * 4 + 1, l); }
    void ble(Label l, int crf = 0) { bc(4, crf * 4 + 1, l); }
    void bdnz(Label l) { bc(16, 0, l); }

    // ---- SPE single precision
    void efs(unsigned xo, int d, int a, int b) {
        word((4u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | xo);
    }
    void efsadd(int d, int a, int b) { efs(0x2C0, d, a, b); }
    void efssub(int d, int a, int b) { efs(0x2C1, d, a, b); }
    void efsmul(int d, int a, int b) { efs(0x2C8, d, a, b); }
    void efsdiv(int d, int a, int b) { efs(0x2C9, d, a, b); }
    void efsneg(int d, int a) { efs(0x2C6, d, a, 0); }
    void efsabs(int d, int a) { efs(0x2C4, d, a, 0); }
    void efscmpgt(int crf, int a, int b) { efs(0x2CC, crf << 2, a, b); }
    void efscmplt(int crf, int a, int b) { efs(0x2CD, crf << 2, a, b); }
    void efscmpeq(int crf, int a, int b) { efs(0x2CE, crf << 2, a, b); }
    void efststgt(int crf, int a, int b) { efs(0x2DC, crf << 2, a, b); }
    void efscfsi(int d, int b) { efs(0x2D1, d, 0, b); }
    void efscfui(int d, int b) { efs(0x2D0, d, 0, b); }
    void efsctsi(int d, int b) { efs(0x2D5, d, 0, b); }
    void efsctsiz(int d, int b) { efs(0x2DA, d, 0, b); }
    void efsctui(int d, int b) { efs(0x2D4, d, 0, b); }
    void efscfsf(int d, int b) { efs(0x2D3, d, 0, b); }
    void efsctsf(int d, int b) { efs(0x2D7, d, 0, b); }

    // The program, with every label resolved. Throws std::logic_error for an unbound label or a branch out of range.
    std::vector<uint32_t> words() const {
        std::vector<uint32_t> out = words_;
        for (const Fixup& f : fixups_) {
            const uint32_t target = labels_.at(f.label);
            if (target == kUnbound) {
                throw std::logic_error("e200::Asm: unbound label");
            }
            const int64_t disp = int64_t(target) - int64_t(origin_ + f.index * 4);
            if (f.conditional) {
                if (disp < -32768 || disp > 32767) {
                    throw std::logic_error("e200::Asm: conditional branch out of range");
                }
                out[f.index] |= uint32_t(disp) & 0xFFFC;
            } else {
                if (disp < -(int64_t(1) << 25) || disp >= (int64_t(1) << 25)) {
                    throw std::logic_error("e200::Asm: branch out of range");
                }
                out[f.index] |= uint32_t(disp) & 0x03FFFFFC;
            }
        }
        return out;
    }

    // words() in memory order (big-endian)
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out;
        for (uint32_t w : words()) {
            out.push_back(uint8_t(w >> 24));
            out.push_back(uint8_t(w >> 16));
            out.push_back(uint8_t(w >> 8));
            out.push_back(uint8_t(w));
        }
        return out;
    }

   private:
    static constexpr uint32_t kUnbound = 0xFFFFFFFF;

    struct Fixup {
        size_t index;
        Label label;
        bool conditional;
    };

    static uint32_t spr_field(int spr) {
        const uint32_t n = uint32_t(spr);
        return ((n & 0x1F) << 16) | (((n >> 5) & 0x1F) << 11);
    }

    void d_form(uint32_t op, int d, int a, uint16_t imm) {
        word((op << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | imm);
    }
    void x(uint32_t xo_, int d, int a, int b, bool rc) {
        word((31u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (xo_ << 1) |
             (rc ? 1u : 0u));
    }
    void xo(uint32_t xo_, int d, int a, int b, bool rc, bool oe) {
        word((31u << 26) | (uint32_t(d) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (oe ? 0x400u : 0u) |
             (xo_ << 1) | (rc ? 1u : 0u));
    }
    void m_form(uint32_t op, int s, int a, int b, int mb, int me, bool rc) {
        word((op << 26) | (uint32_t(s) << 21) | (uint32_t(a) << 16) | (uint32_t(b) << 11) | (uint32_t(mb) << 6) |
             (uint32_t(me) << 1) | (rc ? 1u : 0u));
    }
    void branch(Label l, bool link) {
        fixups_.push_back({words_.size(), l, false});
        word((18u << 26) | (link ? 1u : 0u));
    }

    uint32_t origin_;
    std::vector<uint32_t> words_;
    std::vector<uint32_t> labels_;
    std::vector<Fixup> fixups_;
};

}  // namespace sim::e200
