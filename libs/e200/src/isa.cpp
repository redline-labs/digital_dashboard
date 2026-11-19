#include "e200/isa.h"

#include <array>
#include <cstdio>
#include <string>

namespace sim::e200 {

namespace {

uint8_t bits(uint32_t word, unsigned first, unsigned count) {
    // Book E numbering: bit 0 is the most significant
    return uint8_t((word >> (32 - first - count)) & ((1u << count) - 1));
}

int32_t sext16(uint32_t v) {
    return int32_t(int16_t(uint16_t(v)));
}

Insn base(uint32_t word, Op op) {
    Insn in;
    in.op = op;
    in.word = word;
    in.d = bits(word, 6, 5);
    in.a = bits(word, 11, 5);
    in.b = bits(word, 16, 5);
    in.c = bits(word, 21, 5);
    in.e = bits(word, 26, 5);
    return in;
}

Insn d_form(uint32_t word, Op op, bool signed_imm) {
    Insn in = base(word, op);
    in.imm = signed_imm ? sext16(word & 0xFFFF) : int32_t(word & 0xFFFF);
    return in;
}

Insn x_form(uint32_t word, Op op, bool rc_allowed) {
    Insn in = base(word, op);
    if (rc_allowed && (word & 1)) {
        in.flags |= flag::kRc;
    }
    return in;
}

Insn xo_form(uint32_t word, Op op) {
    Insn in = base(word, op);
    if (word & 1) {
        in.flags |= flag::kRc;
    }
    if (word & 0x400) {
        in.flags |= flag::kOe;
    }
    return in;
}

Insn illegal(uint32_t word) {
    Insn in;
    in.word = word;
    return in;
}

Insn decode_19(uint32_t word) {
    const unsigned xo = (word >> 1) & 0x3FF;
    switch (xo) {
        case 0:
            return base(word, Op::Mcrf);
        case 16: {
            Insn in = base(word, Op::Bclr);
            if (word & 1) {
                in.flags |= flag::kLk;
            }
            return in;
        }
        case 528: {
            Insn in = base(word, Op::Bcctr);
            if (word & 1) {
                in.flags |= flag::kLk;
            }
            return in;
        }
        case 33:
            return base(word, Op::Crnor);
        case 129:
            return base(word, Op::Crandc);
        case 193:
            return base(word, Op::Crxor);
        case 225:
            return base(word, Op::Crnand);
        case 257:
            return base(word, Op::Crand);
        case 289:
            return base(word, Op::Creqv);
        case 417:
            return base(word, Op::Crorc);
        case 449:
            return base(word, Op::Cror);
        case 50:
            return base(word, Op::Rfi);
        case 51:
            return base(word, Op::Rfci);
        case 39:
            return base(word, Op::Rfdi);
        case 38:
            return base(word, Op::Rfmci);
        case 150:
            return base(word, Op::Isync);
        default:
            return illegal(word);
    }
}

Insn decode_31(uint32_t word) {
    // isel: A-form, the condition bit in 21-25 and XO 15 in bits 26-30
    if (((word >> 1) & 0x1F) == 15) {
        return base(word, Op::Isel);
    }
    // XO-form arithmetic (OE in bit 21: the XO is bits 22-30)
    switch ((word >> 1) & 0x1FF) {
        case 266:
            return xo_form(word, Op::Add);
        case 10:
            return xo_form(word, Op::Addc);
        case 138:
            return xo_form(word, Op::Adde);
        case 234:
            return xo_form(word, Op::Addme);
        case 202:
            return xo_form(word, Op::Addze);
        case 40:
            return xo_form(word, Op::Subf);
        case 8:
            return xo_form(word, Op::Subfc);
        case 136:
            return xo_form(word, Op::Subfe);
        case 232:
            return xo_form(word, Op::Subfme);
        case 200:
            return xo_form(word, Op::Subfze);
        case 104:
            return xo_form(word, Op::Neg);
        case 235:
            return xo_form(word, Op::Mullw);
        case 75:
            return xo_form(word, Op::Mulhw);
        case 11:
            return xo_form(word, Op::Mulhwu);
        case 491:
            return xo_form(word, Op::Divw);
        case 459:
            return xo_form(word, Op::Divwu);
        default:
            break;
    }
    const unsigned xo = (word >> 1) & 0x3FF;
    switch (xo) {
        case 28:
            return x_form(word, Op::And, true);
        case 60:
            return x_form(word, Op::Andc, true);
        case 444:
            return x_form(word, Op::Or, true);
        case 412:
            return x_form(word, Op::Orc, true);
        case 316:
            return x_form(word, Op::Xor, true);
        case 476:
            return x_form(word, Op::Nand, true);
        case 124:
            return x_form(word, Op::Nor, true);
        case 284:
            return x_form(word, Op::Eqv, true);
        case 954:
            return x_form(word, Op::Extsb, true);
        case 922:
            return x_form(word, Op::Extsh, true);
        case 26:
            return x_form(word, Op::Cntlzw, true);
        case 24:
            return x_form(word, Op::Slw, true);
        case 536:
            return x_form(word, Op::Srw, true);
        case 792:
            return x_form(word, Op::Sraw, true);
        case 824:
            return x_form(word, Op::Srawi, true);
        case 0:
            return base(word, Op::Cmp);
        case 32:
            return base(word, Op::Cmpl);
        case 19:
            return base(word, Op::Mfcr);
        case 144: {
            Insn in = base(word, Op::Mtcrf);
            in.imm = int32_t((word >> 12) & 0xFF);
            return in;
        }
        case 512:
            return base(word, Op::Mcrxr);
        case 87:
            return base(word, Op::Lbzx);
        case 119:
            return base(word, Op::Lbzux);
        case 279:
            return base(word, Op::Lhzx);
        case 311:
            return base(word, Op::Lhzux);
        case 343:
            return base(word, Op::Lhax);
        case 375:
            return base(word, Op::Lhaux);
        case 23:
            return base(word, Op::Lwzx);
        case 55:
            return base(word, Op::Lwzux);
        case 215:
            return base(word, Op::Stbx);
        case 247:
            return base(word, Op::Stbux);
        case 407:
            return base(word, Op::Sthx);
        case 439:
            return base(word, Op::Sthux);
        case 151:
            return base(word, Op::Stwx);
        case 183:
            return base(word, Op::Stwux);
        case 790:
            return base(word, Op::Lhbrx);
        case 534:
            return base(word, Op::Lwbrx);
        case 918:
            return base(word, Op::Sthbrx);
        case 662:
            return base(word, Op::Stwbrx);
        case 20:
            return base(word, Op::Lwarx);
        case 150:
            return (word & 1) ? x_form(word, Op::StwcxRc, true) : illegal(word);
        case 4:
            return base(word, Op::Tw);
        case 339:
        case 467: {
            Insn in = base(word, xo == 339 ? Op::Mfspr : Op::Mtspr);
            in.spr = uint16_t(((word >> 16) & 0x1F) | (((word >> 11) & 0x1F) << 5));
            return in;
        }
        case 83:
            return base(word, Op::Mfmsr);
        case 146:
            return base(word, Op::Mtmsr);
        case 131:
            return base(word, Op::Wrtee);
        case 163: {
            Insn in = base(word, Op::Wrteei);
            in.imm = int32_t((word >> 15) & 1);
            return in;
        }
        case 62:
            return base(word, Op::Wait);
        case 598:
            return base(word, Op::Msync);
        case 854:
            return base(word, Op::Mbar);
        case 1014:
            return base(word, Op::Dcbz);
        case 86:   // dcbf
        case 54:   // dcbst
        case 278:  // dcbt
        case 246:  // dcbtst
        case 470:  // dcbi
        case 982:  // icbi
        case 22:   // icbt
        case 166:  // dcbtls (the cache-locking forms)
        case 134:  // dcbtstls
        case 486:  // icbtls
        case 390:  // dcblc
        case 230:  // icblc
            return base(word, Op::CacheNop);
        case 978:
            return base(word, Op::Tlbwe);
        case 946:
            return base(word, Op::Tlbre);
        case 914:
            return base(word, Op::Tlbsx);
        case 786:
            return base(word, Op::Tlbivax);
        case 566:
            return base(word, Op::Tlbsync);
        default:
            return illegal(word);
    }
}

Insn decode_spe(uint32_t word) {
    switch (word & 0x7FF) {
        case 0x2C0:
            return base(word, Op::Efsadd);
        case 0x2C1:
            return base(word, Op::Efssub);
        case 0x2C4:
            return base(word, Op::Efsabs);
        case 0x2C5:
            return base(word, Op::Efsnabs);
        case 0x2C6:
            return base(word, Op::Efsneg);
        case 0x2C8:
            return base(word, Op::Efsmul);
        case 0x2C9:
            return base(word, Op::Efsdiv);
        case 0x2CC:
            return base(word, Op::Efscmpgt);
        case 0x2CD:
            return base(word, Op::Efscmplt);
        case 0x2CE:
            return base(word, Op::Efscmpeq);
        case 0x2D0:
            return base(word, Op::Efscfui);
        case 0x2D1:
            return base(word, Op::Efscfsi);
        case 0x2D2:
            return base(word, Op::Efscfuf);
        case 0x2D3:
            return base(word, Op::Efscfsf);
        case 0x2D4:
            return base(word, Op::Efsctui);
        case 0x2D5:
            return base(word, Op::Efsctsi);
        case 0x2D6:
            return base(word, Op::Efsctuf);
        case 0x2D7:
            return base(word, Op::Efsctsf);
        case 0x2D8:
            return base(word, Op::Efsctuiz);
        case 0x2DA:
            return base(word, Op::Efsctsiz);
        case 0x2DC:
            return base(word, Op::Efststgt);
        case 0x2DD:
            return base(word, Op::Efststlt);
        case 0x2DE:
            return base(word, Op::Efststeq);
        default:
            return illegal(word);
    }
}

}  // namespace

Insn decode(uint32_t word) {
    switch (word >> 26) {
        case 3:
            return d_form(word, Op::Twi, true);
        case 4:
            return decode_spe(word);
        case 7:
            return d_form(word, Op::Mulli, true);
        case 8:
            return d_form(word, Op::Subfic, true);
        case 10:
            return d_form(word, Op::Cmpli, false);
        case 11:
            return d_form(word, Op::Cmpi, true);
        case 12:
            return d_form(word, Op::Addic, true);
        case 13:
            return d_form(word, Op::AddicRc, true);
        case 14:
            return d_form(word, Op::Addi, true);
        case 15:
            return d_form(word, Op::Addis, true);
        case 16: {
            Insn in = base(word, Op::Bc);
            in.imm = sext16(word & 0xFFFC);
            in.flags = uint8_t(((word & 1) ? flag::kLk : 0) | ((word & 2) ? flag::kAa : 0));
            return in;
        }
        case 17:
            return (word & 2) ? base(word, Op::Sc) : illegal(word);
        case 18: {
            Insn in = base(word, Op::B);
            in.imm = int32_t((word & 0x03FFFFFC) << 6) >> 6;  // 26-bit signed displacement
            in.flags = uint8_t(((word & 1) ? flag::kLk : 0) | ((word & 2) ? flag::kAa : 0));
            return in;
        }
        case 19:
            return decode_19(word);
        case 20:
            return x_form(word, Op::Rlwimi, true);
        case 21:
            return x_form(word, Op::Rlwinm, true);
        case 23:
            return x_form(word, Op::Rlwnm, true);
        case 24:
            return d_form(word, Op::Ori, false);
        case 25:
            return d_form(word, Op::Oris, false);
        case 26:
            return d_form(word, Op::Xori, false);
        case 27:
            return d_form(word, Op::Xoris, false);
        case 28:
            return d_form(word, Op::AndiRc, false);
        case 29:
            return d_form(word, Op::AndisRc, false);
        case 31:
            return decode_31(word);
        case 32:
            return d_form(word, Op::Lwz, true);
        case 33:
            return d_form(word, Op::Lwzu, true);
        case 34:
            return d_form(word, Op::Lbz, true);
        case 35:
            return d_form(word, Op::Lbzu, true);
        case 36:
            return d_form(word, Op::Stw, true);
        case 37:
            return d_form(word, Op::Stwu, true);
        case 38:
            return d_form(word, Op::Stb, true);
        case 39:
            return d_form(word, Op::Stbu, true);
        case 40:
            return d_form(word, Op::Lhz, true);
        case 41:
            return d_form(word, Op::Lhzu, true);
        case 42:
            return d_form(word, Op::Lha, true);
        case 43:
            return d_form(word, Op::Lhau, true);
        case 44:
            return d_form(word, Op::Sth, true);
        case 45:
            return d_form(word, Op::Sthu, true);
        case 46:
            return d_form(word, Op::Lmw, true);
        case 47:
            return d_form(word, Op::Stmw, true);
        default:
            return illegal(word);
    }
}

bool ends_block(const Insn& in) {
    static constexpr Op kEnders[] = {
        Op::B,     Op::Bc,    Op::Bclr,  Op::Bcctr, Op::Sc,    Op::Tw,      Op::Twi,     Op::Mtmsr,
        Op::Wrtee, Op::Wrteei, Op::Rfi,  Op::Rfci,  Op::Rfdi,    Op::Rfmci,   Op::Wait,
        Op::Isync, Op::Tlbwe, Op::Tlbivax, Op::Tlbsync, Op::Illegal, Op::Count_,
    };
    if (in.op == Op::Mtspr) {
        // moves to LR, CTR and XER change nothing a block depends on
        return in.spr != 1 && in.spr != 8 && in.spr != 9;
    }
    for (Op op : kEnders) {
        if (in.op == op) {
            return true;
        }
    }
    return false;
}

namespace {

constexpr std::array<const char*, size_t(Op::Count_)> kNames = {
    "illegal",
    "addi", "addis", "addic", "addic.", "subfic", "mulli",
    "add", "addc", "adde", "addme", "addze", "subf", "subfc", "subfe", "subfme", "subfze", "neg",
    "mullw", "mulhw", "mulhwu", "divw", "divwu",
    "andi.", "andis.", "ori", "oris", "xori", "xoris",
    "and", "andc", "or", "orc", "xor", "nand", "nor", "eqv", "extsb", "extsh", "cntlzw",
    "rlwinm", "rlwnm", "rlwimi", "slw", "srw", "sraw", "srawi",
    "cmp", "cmpi", "cmpl", "cmpli",
    "b", "bc", "bclr", "bcctr",
    "mcrf", "crand", "crandc", "creqv", "crnand", "crnor", "cror", "crorc", "crxor", "mfcr", "mtcrf", "mcrxr",
    "isel",
    "lbz", "lbzu", "lbzx", "lbzux", "lhz", "lhzu", "lhzx", "lhzux", "lha", "lhau", "lhax", "lhaux",
    "lwz", "lwzu", "lwzx", "lwzux",
    "stb", "stbu", "stbx", "stbux", "sth", "sthu", "sthx", "sthux", "stw", "stwu", "stwx", "stwux",
    "lhbrx", "lwbrx", "sthbrx", "stwbrx", "lmw", "stmw", "lwarx", "stwcx.",
    "sc", "tw", "twi", "mfspr", "mtspr", "mfmsr", "mtmsr", "wrtee", "wrteei", "rfi", "rfci", "rfdi", "rfmci", "wait",
    "isync", "msync", "mbar", "dcache", "dcbz",
    "tlbwe", "tlbre", "tlbsx", "tlbivax", "tlbsync",
    "efsadd", "efssub", "efsmul", "efsdiv", "efsabs", "efsnabs", "efsneg",
    "efscmpgt", "efscmplt", "efscmpeq", "efststgt", "efststlt", "efststeq",
    "efscfsi", "efscfui", "efscfsf", "efscfuf", "efsctsi", "efsctui", "efsctsiz", "efsctuiz", "efsctsf", "efsctuf",
};

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

std::string r(unsigned n) {
    return "r" + std::to_string(n);
}

}  // namespace

const char* mnemonic(const Insn& in) {
    const size_t n = size_t(in.op);
    return n < kNames.size() ? kNames[n] : "?";
}

std::string disassemble(const Insn& in, uint32_t pc) {
    std::string m = mnemonic(in);
    const bool rc = in.has(flag::kRc) && in.op != Op::AddicRc && in.op != Op::AndiRc && in.op != Op::AndisRc &&
                    in.op != Op::StwcxRc;
    if (in.has(flag::kOe)) {
        m += "o";
    }
    if (rc) {
        m += ".";
    }
    const std::string d = std::to_string(in.imm);
    switch (in.op) {
        case Op::Illegal:
        case Op::Count_:
            return ".long " + hex(in.word);
        case Op::Addi:
        case Op::Addis:
        case Op::Addic:
        case Op::AddicRc:
        case Op::Subfic:
        case Op::Mulli:
            return m + " " + r(in.d) + "," + r(in.a) + "," + d;
        case Op::AndiRc:
        case Op::AndisRc:
        case Op::Ori:
        case Op::Oris:
        case Op::Xori:
        case Op::Xoris:
            return m + " " + r(in.a) + "," + r(in.d) + "," + hex(uint32_t(in.imm));
        case Op::Add:
        case Op::Addc:
        case Op::Adde:
        case Op::Subf:
        case Op::Subfc:
        case Op::Subfe:
        case Op::Mullw:
        case Op::Mulhw:
        case Op::Mulhwu:
        case Op::Divw:
        case Op::Divwu:
            return m + " " + r(in.d) + "," + r(in.a) + "," + r(in.b);
        case Op::Addme:
        case Op::Addze:
        case Op::Subfme:
        case Op::Subfze:
        case Op::Neg:
            return m + " " + r(in.d) + "," + r(in.a);
        case Op::And:
        case Op::Andc:
        case Op::Or:
        case Op::Orc:
        case Op::Xor:
        case Op::Nand:
        case Op::Nor:
        case Op::Eqv:
        case Op::Slw:
        case Op::Srw:
        case Op::Sraw:
            return m + " " + r(in.a) + "," + r(in.d) + "," + r(in.b);
        case Op::Extsb:
        case Op::Extsh:
        case Op::Cntlzw:
            return m + " " + r(in.a) + "," + r(in.d);
        case Op::Srawi:
            return m + " " + r(in.a) + "," + r(in.d) + "," + std::to_string(in.b);
        case Op::Rlwinm:
        case Op::Rlwimi:
            return m + " " + r(in.a) + "," + r(in.d) + "," + std::to_string(in.b) + "," + std::to_string(in.c) + "," +
                   std::to_string(in.e);
        case Op::Rlwnm:
            return m + " " + r(in.a) + "," + r(in.d) + "," + r(in.b) + "," + std::to_string(in.c) + "," +
                   std::to_string(in.e);
        case Op::Cmp:
        case Op::Cmpl:
            return m + " cr" + std::to_string(in.d >> 2) + "," + r(in.a) + "," + r(in.b);
        case Op::Cmpi:
        case Op::Cmpli:
            return m + " cr" + std::to_string(in.d >> 2) + "," + r(in.a) + "," + d;
        case Op::B: {
            const uint32_t target = in.has(flag::kAa) ? uint32_t(in.imm) : pc + uint32_t(in.imm);
            return std::string(in.has(flag::kLk) ? "bl" : "b") + (in.has(flag::kAa) ? "a " : " ") + hex(target);
        }
        case Op::Bc: {
            const uint32_t target = in.has(flag::kAa) ? uint32_t(in.imm) : pc + uint32_t(in.imm);
            return std::string(in.has(flag::kLk) ? "bcl " : "bc ") + std::to_string(in.d) + "," +
                   std::to_string(in.a) + "," + hex(target);
        }
        case Op::Bclr:
        case Op::Bcctr:
            return m + (in.has(flag::kLk) ? "l " : " ") + std::to_string(in.d) + "," + std::to_string(in.a);
        case Op::Mcrf:
            return m + " cr" + std::to_string(in.d >> 2) + ",cr" + std::to_string(in.a >> 2);
        case Op::Crand:
        case Op::Crandc:
        case Op::Creqv:
        case Op::Crnand:
        case Op::Crnor:
        case Op::Cror:
        case Op::Crorc:
        case Op::Crxor:
            return m + " " + std::to_string(in.d) + "," + std::to_string(in.a) + "," + std::to_string(in.b);
        case Op::Mfcr:
        case Op::Mfmsr:
        case Op::Mtmsr:
        case Op::Wrtee:
            return m + " " + r(in.d);
        case Op::Mtcrf:
            return m + " " + hex(uint32_t(in.imm)) + "," + r(in.d);
        case Op::Mcrxr:
            return m + " cr" + std::to_string(in.d >> 2);
        case Op::Isel:
            return m + " " + r(in.d) + "," + r(in.a) + "," + r(in.b) + "," + std::to_string(in.c);
        case Op::Lbz:
        case Op::Lbzu:
        case Op::Lhz:
        case Op::Lhzu:
        case Op::Lha:
        case Op::Lhau:
        case Op::Lwz:
        case Op::Lwzu:
        case Op::Stb:
        case Op::Stbu:
        case Op::Sth:
        case Op::Sthu:
        case Op::Stw:
        case Op::Stwu:
        case Op::Lmw:
        case Op::Stmw:
            return m + " " + r(in.d) + "," + d + "(" + r(in.a) + ")";
        case Op::Lbzx:
        case Op::Lbzux:
        case Op::Lhzx:
        case Op::Lhzux:
        case Op::Lhax:
        case Op::Lhaux:
        case Op::Lwzx:
        case Op::Lwzux:
        case Op::Stbx:
        case Op::Stbux:
        case Op::Sthx:
        case Op::Sthux:
        case Op::Stwx:
        case Op::Stwux:
        case Op::Lhbrx:
        case Op::Lwbrx:
        case Op::Sthbrx:
        case Op::Stwbrx:
        case Op::Lwarx:
        case Op::StwcxRc:
            return m + " " + r(in.d) + "," + r(in.a) + "," + r(in.b);
        case Op::Tw:
            return m + " " + std::to_string(in.d) + "," + r(in.a) + "," + r(in.b);
        case Op::Twi:
            return m + " " + std::to_string(in.d) + "," + r(in.a) + "," + d;
        case Op::Mfspr:
            return m + " " + r(in.d) + "," + std::to_string(in.spr);
        case Op::Mtspr:
            return m + " " + std::to_string(in.spr) + "," + r(in.d);
        case Op::Wrteei:
            return m + " " + d;
        case Op::Dcbz:
        case Op::CacheNop:
        case Op::Tlbsx:
        case Op::Tlbivax:
            return m + " " + r(in.a) + "," + r(in.b);
        case Op::Sc:
        case Op::Rfi:
        case Op::Rfci:
        case Op::Rfdi:
        case Op::Rfmci:
        case Op::Wait:
        case Op::Isync:
        case Op::Msync:
        case Op::Mbar:
        case Op::Tlbwe:
        case Op::Tlbre:
        case Op::Tlbsync:
            return m;
        case Op::Efsadd:
        case Op::Efssub:
        case Op::Efsmul:
        case Op::Efsdiv:
            return m + " " + r(in.d) + "," + r(in.a) + "," + r(in.b);
        case Op::Efsabs:
        case Op::Efsnabs:
        case Op::Efsneg:
            return m + " " + r(in.d) + "," + r(in.a);
        case Op::Efscmpgt:
        case Op::Efscmplt:
        case Op::Efscmpeq:
        case Op::Efststgt:
        case Op::Efststlt:
        case Op::Efststeq:
            return m + " cr" + std::to_string(in.d >> 2) + "," + r(in.a) + "," + r(in.b);
        case Op::Efscfsi:
        case Op::Efscfui:
        case Op::Efscfsf:
        case Op::Efscfuf:
        case Op::Efsctsi:
        case Op::Efsctui:
        case Op::Efsctsiz:
        case Op::Efsctuiz:
        case Op::Efsctsf:
        case Op::Efsctuf:
            return m + " " + r(in.d) + "," + r(in.b);
    }
    return m;
}

}  // namespace sim::e200
