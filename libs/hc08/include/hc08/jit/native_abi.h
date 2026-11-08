// What generated code, the helpers and the engine share: the context a block
// is called with, at fixed offsets the host assemblers read.
//
// A block is `void block(JitContext*)`. It keeps A, HX, SP, CCR and the cycle count in host registers (loaded at
// entry, written back at a real exit and around helper calls), reads RAM/flash/ROM bytes straight from `memory` when
// `regions[addr]` is not a device window, writes RAM straight when the page holds no decoded code, and calls the
// helpers otherwise. A helper access sets `exit`: the block leaves after the instruction that made it, so the engine
// re-checks interrupts and the cycle limit exactly where the interpreter would.
//
// Chaining: at a block's end the next pc is looked up in `table` (one ChainEntry* per pc, null where nothing is
// translated or the block was dropped). If there is a block, `exit` is clear, its cycles fit under `*limit` and no
// interrupt can be taken (`*irq_lines` zero or CCR.I set), the code jumps straight into that block's body with the
// guest registers still in host registers: the engine loop's own checks, made in native code. Otherwise it leaves.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hc08/cpu_state.h"
#include "hc08/isa.h"

namespace sim::jit {

struct JitContext;

using ReadHelper = uint32_t (*)(JitContext*, uint32_t addr, uint32_t pc);
using WriteHelper = void (*)(JitContext*, uint32_t addr, uint32_t value, uint32_t pc);
// runs one instruction through execute_insn: 1 = the block may continue, 0 = leave (pc is set; has_stop if it stopped)
using StepHelper = uint32_t (*)(JitContext*, const Insn* insn, uint32_t remaining_cycles);
using BlockEntry = void (*)(JitContext*);

// What a chained exit needs of the next block: where its body starts (after the prologue) and its cycle cost.
struct ChainEntry {
    void* body = nullptr;
    uint64_t cycles = 0;
};

struct JitContext {
    CpuState* state = nullptr;
    uint8_t* memory = nullptr;            // the 64 KB storage
    const uint8_t* regions = nullptr;     // 64 KB of sim::Region tags
    const uint8_t* code_pages = nullptr;  // 256 bytes: 1 where the page holds decoded code
    void* env = nullptr;
    ReadHelper read = nullptr;
    WriteHelper write = nullptr;
    StepHelper step = nullptr;
    uint32_t exit = 0;  // set by a helper access or an invalidation: leave after this instruction
    uint32_t has_stop = 0;
    Stop stop;
    uint32_t spill[8] = {};               // the scratch registers around a helper call
    ChainEntry* const* table = nullptr;   // 65536 entries
    const uint64_t* limit = nullptr;      // the environment's cycle limit
    const uint32_t* irq_lines = nullptr;  // non-zero: an interrupt source is asserted
};

inline constexpr int32_t kCtxState = int32_t(offsetof(JitContext, state));
inline constexpr int32_t kCtxMemory = int32_t(offsetof(JitContext, memory));
inline constexpr int32_t kCtxRegions = int32_t(offsetof(JitContext, regions));
inline constexpr int32_t kCtxCodePages = int32_t(offsetof(JitContext, code_pages));
inline constexpr int32_t kCtxRead = int32_t(offsetof(JitContext, read));
inline constexpr int32_t kCtxWrite = int32_t(offsetof(JitContext, write));
inline constexpr int32_t kCtxStep = int32_t(offsetof(JitContext, step));
inline constexpr int32_t kCtxExit = int32_t(offsetof(JitContext, exit));
inline constexpr int32_t kCtxSpill = int32_t(offsetof(JitContext, spill));
inline constexpr int32_t kCtxTable = int32_t(offsetof(JitContext, table));
inline constexpr int32_t kCtxLimit = int32_t(offsetof(JitContext, limit));
inline constexpr int32_t kCtxIrqLines = int32_t(offsetof(JitContext, irq_lines));
inline constexpr int32_t kChainBody = int32_t(offsetof(ChainEntry, body));
inline constexpr int32_t kChainCycles = int32_t(offsetof(ChainEntry, cycles));

inline constexpr int32_t kStateA = int32_t(offsetof(CpuState, a));
inline constexpr int32_t kStateHx = int32_t(offsetof(CpuState, hx));
inline constexpr int32_t kStateSp = int32_t(offsetof(CpuState, sp));
inline constexpr int32_t kStatePc = int32_t(offsetof(CpuState, pc));
inline constexpr int32_t kStateCcr = int32_t(offsetof(CpuState, ccr));
inline constexpr int32_t kStateCycles = int32_t(offsetof(CpuState, cycles));

}  // namespace sim::jit
