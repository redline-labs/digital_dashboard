#include "hc08/isa.h"

namespace sim {

const char* mnemonic(Op op) {
    static constexpr const char* kNames[] = {
        "illegal", "brset", "brclr", "bset", "bclr", "bra",  "brn",  "bhi",  "bls",  "bcc",  "bcs",  "bne",  "beq",
        "bhcc",    "bhcs",  "bpl",   "bmi",  "bmc",  "bms",  "bil",  "bih",  "bge",  "blt",  "bgt",  "ble",  "bsr",
        "neg",     "com",   "lsr",   "ror",  "asr",  "lsl",  "rol",  "dec",  "inc",  "tst",  "clr",  "cbeq", "dbnz",
        "mov",     "ldhx",  "sthx",  "cphx", "aix",  "ais",  "sub",  "cmp",  "sbc",  "cpx",  "and",  "bit",  "lda",
        "sta",     "eor",   "adc",   "ora",  "add",  "jmp",  "jsr",  "ldx",  "stx",  "mul",  "div",  "nsa",  "daa",
        "rti",     "rts",   "swi",   "tap",  "tpa",  "pula", "psha", "pulx", "pshx", "pulh", "pshh", "clrh", "stop",
        "wait",    "txs",   "tsx",   "tax",  "txa",  "clc",  "sec",  "cli",  "sei",  "rsp",  "nop",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(Op::Nop) + 1);
    return kNames[size_t(op)];
}

}  // namespace sim
