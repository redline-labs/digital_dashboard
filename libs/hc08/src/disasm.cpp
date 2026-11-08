#include "hc08/disasm.h"

#include <cstdio>

namespace sim {

namespace {

std::string hex8(uint8_t v) {
    char text[8];
    std::snprintf(text, sizeof(text), "$%02x", v);
    return text;
}

std::string hex16(uint16_t v) {
    char text[8];
    std::snprintf(text, sizeof(text), "$%04x", v);
    return text;
}

std::string target(uint16_t pc, const Insn& in, uint8_t rel) {
    return hex16(uint16_t(pc + in.length + sext8(rel)));
}

}  // namespace

std::string disassemble(const Insn& in, uint16_t pc) {
    std::string m = mnemonic(in.op);

    if (in.op == Op::Illegal) {
        return "illegal " + (in.b[0] == 0x9E ? hex16(uint16_t(0x9E00 | in.b[1])) : hex8(in.b[1]));
    }

    switch (in.op) {
        case Op::Brset:
        case Op::Brclr:
            return m + " " + std::to_string(in.bit) + "," + hex8(in.b[0]) + "," + target(pc, in, in.b[1]);
        case Op::Bset:
        case Op::Bclr:
            return m + " " + std::to_string(in.bit) + "," + hex8(in.b[0]);
        case Op::Illegal:
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

    if (in.mode == Mode::Rel) {
        return m + " " + target(pc, in, in.b[0]);
    }

    if (in.op == Op::Cbeq || in.op == Op::Dbnz) {
        switch (in.mode) {
            case Mode::InhA:
                return m + (in.op == Op::Cbeq ? "a #" + hex8(in.b[0]) + "," + target(pc, in, in.b[1])
                                              : "a " + target(pc, in, in.b[0]));
            case Mode::InhX:
                return m + (in.op == Op::Cbeq ? "x #" + hex8(in.b[0]) + "," + target(pc, in, in.b[1])
                                              : "x " + target(pc, in, in.b[0]));
            case Mode::Dir:
                return m + " " + hex8(in.b[0]) + "," + target(pc, in, in.b[1]);
            case Mode::Ix:
                return m + " ,x," + target(pc, in, in.b[0]);
            case Mode::IxPlus:
                return m + " ,x+," + target(pc, in, in.b[0]);
            case Mode::Ix1:
                return m + " " + hex8(in.b[0]) + ",x," + target(pc, in, in.b[1]);
            case Mode::Ix1Plus:
                return m + " " + hex8(in.b[0]) + ",x+," + target(pc, in, in.b[1]);
            case Mode::Sp1:
                return m + " " + hex8(in.b[0]) + ",sp," + target(pc, in, in.b[1]);
            case Mode::None:
            case Mode::Inh:
            case Mode::Imm:
            case Mode::Imm16:
            case Mode::Ext:
            case Mode::Ix2:
            case Mode::Sp2:
            case Mode::Rel:
            case Mode::DirDir:
            case Mode::DirIxP:
            case Mode::ImmDir:
                break;
        }
    }

    switch (in.mode) {
        case Mode::Inh:
            return m;
        case Mode::InhA:
            return m + "a";
        case Mode::InhX:
            return m + "x";
        case Mode::Imm:
            return m + " #" + hex8(in.b[0]);
        case Mode::Imm16:
            return m + " #" + hex16(in.word());
        case Mode::Dir:
            return m + " " + hex8(in.b[0]);
        case Mode::Ext:
            return m + " " + hex16(in.word());
        case Mode::Ix:
            return m + " ,x";
        case Mode::Ix1:
            return m + " " + hex8(in.b[0]) + ",x";
        case Mode::Ix2:
            return m + " " + hex16(in.word()) + ",x";
        case Mode::Sp1:
            return m + " " + hex8(in.b[0]) + ",sp";
        case Mode::Sp2:
            return m + " " + hex16(in.word()) + ",sp";
        case Mode::IxPlus:  // MOV ,X+,opr8
            return m + " ,x+," + hex8(in.b[0]);
        case Mode::DirDir:
            return m + " " + hex8(in.b[0]) + "," + hex8(in.b[1]);
        case Mode::DirIxP:
            return m + " " + hex8(in.b[0]) + ",x+";
        case Mode::ImmDir:
            return m + " #" + hex8(in.b[0]) + "," + hex8(in.b[1]);
        case Mode::None:
        case Mode::Rel:
        case Mode::Ix1Plus:
            return m;
    }
}

}  // namespace sim
