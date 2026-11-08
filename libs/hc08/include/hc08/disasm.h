// One-line disassembly of a CPU08 instruction, for traces, the debugger and the agent ("ldhx #$0114").
#pragma once

#include <cstdint>
#include <string>

#include "hc08/isa.h"

namespace sim {

// The text of `insn` at `pc` (branch targets resolved to absolute addresses).
std::string disassemble(const Insn& insn, uint16_t pc);

}  // namespace sim
