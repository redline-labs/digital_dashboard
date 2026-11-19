// The e200 JIT: blocks of decoded instructions as host code.
//
// A block is up to kMaxBlockInstructions instructions from one 4 KB page, ending at the first instruction ends_block()
// names (e200/isa.h). It is translated for one effective address and one physical page (the branch targets and LR
// values are effective, the code is the physical page's), the first time it runs there.
//
// The guest registers live in the CpuState; a block keeps the ones it uses most (up to kCachedRegisters of the GPRs,
// CR, CTR, LR) in host registers, loaded at its body and written back at every exit and around every step-helper call.
// Integer arithmetic and logic, compares (through the host's flags), rotates, isel, the LR and CTR moves, the loads
// and stores and every branch have a native form; the SPE arithmetic is a direct call (it cannot fault or branch);
// everything else is run by the step helper, which calls the interpreter's execute_insn. A load or store looks its
// page up in the machine's software TLB inline; on any miss (a device, a fault, a page crossing, a write to a page of
// code, a reservation that a store may clear) the whole instruction goes to the step helper instead -- nothing has
// been done yet, so the interpreter runs it as if the block were not there.
//
// The engine loop is run_core's (e200/cpu.h), block by block: the decrementer, the cycle limit, interrupts and pc
// watches are checked before each block, and a block is entered only when all its instructions fit before the limit
// and before the decrementer's next event, so no check inside it could have fired. Only a step-helper instruction can
// change that (a device access moves the limit or raises an interrupt input, a write drops code, the instruction
// branches or traps); the helper then says to leave, with the CpuState already where the interpreter would leave it.
//
// Chaining: a block that ends in a native branch or falls through goes straight into the next block when the table
// (by effective address) holds one translated from the page the pc translates to now, and it ends within the budget
// (Context::budget: the cycle limit and the decrementer's event, kept current by the step helper) -- the loop's own
// checks, made in native code. A jump back to the block's own start keeps its cached registers; one within its own
// page skips the translation check. A block whose last instruction went through the step helper (an MSR write, an
// interrupt enabled) always returns to the loop.
//
// The host assembler is AArch64 (asmjit a64). On other hosts host_supported() is false and the machine runs the block
// engine instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "emu/jit/code_memory.h"
#include "e200/cpu_state.h"
#include "e200/isa.h"

namespace sim::e200 {
struct SoftTlb;
}

namespace sim::e200::jit {

inline constexpr int kMaxBlockInstructions = 32;

struct Context;

// Runs `insn` (at effective `pc`, the k-th of its block, 0-based) through execute_insn: 0 = the block may go on (pc is
// pc + 4), 1 = leave (the CpuState holds pc and cycles).
using StepHelper = uint32_t (*)(Context*, const Insn* insn, uint32_t pc, uint32_t k);
using Entry = void (*)(Context*);

struct Block;

// What a chained exit reads of the next block (the table holds these): where it is, its physical page, its length,
// and where its body starts (after the prologue).
struct ChainEntry {
    uint32_t ea = 0;
    uint32_t pa_page = 0;  // physical address >> 12
    uint32_t count = 0;
    uint32_t pad = 0;
    void* body = nullptr;
    Block* block = nullptr;
};

inline constexpr uint32_t kChainTableBits = 14;
inline constexpr uint32_t kChainTableSize = 1u << kChainTableBits;  // by (ea >> 2), direct-mapped

struct Context {
    CpuState* state = nullptr;
    SoftTlb* const* soft = nullptr;  // the machine's current software-TLB set (it changes only in a step helper)
    uint64_t start_cycles = 0;       // CpuState::cycles when the running block was entered
    StepHelper step = nullptr;
    void* env = nullptr;  // the machine
    ChainEntry* const* table = nullptr;  // kChainTableSize entries, null where nothing is translated
    uint64_t budget = 0;     // a chained block runs only if it ends by here (the cycle limit, the decrementer's event)
    uint64_t block_end = 0;  // where the running block ends
};

inline constexpr int32_t kCtxState = int32_t(offsetof(Context, state));
inline constexpr int32_t kCtxSoft = int32_t(offsetof(Context, soft));
inline constexpr int32_t kCtxStartCycles = int32_t(offsetof(Context, start_cycles));
inline constexpr int32_t kCtxStep = int32_t(offsetof(Context, step));
inline constexpr int32_t kCtxTable = int32_t(offsetof(Context, table));
inline constexpr int32_t kCtxBudget = int32_t(offsetof(Context, budget));
inline constexpr int32_t kCtxBlockEnd = int32_t(offsetof(Context, block_end));
inline constexpr int32_t kChainEa = int32_t(offsetof(ChainEntry, ea));
inline constexpr int32_t kChainPaPage = int32_t(offsetof(ChainEntry, pa_page));
inline constexpr int32_t kChainCount = int32_t(offsetof(ChainEntry, count));
inline constexpr int32_t kChainBody = int32_t(offsetof(ChainEntry, body));

inline constexpr int32_t kStateGpr = int32_t(offsetof(CpuState, gpr));
inline constexpr int32_t kStatePc = int32_t(offsetof(CpuState, pc));
inline constexpr int32_t kStateCr = int32_t(offsetof(CpuState, cr));
inline constexpr int32_t kStateXer = int32_t(offsetof(CpuState, xer));
inline constexpr int32_t kStateLr = int32_t(offsetof(CpuState, lr));
inline constexpr int32_t kStateCtr = int32_t(offsetof(CpuState, ctr));
inline constexpr int32_t kStateCycles = int32_t(offsetof(CpuState, cycles));
inline constexpr int32_t kStateReserved = int32_t(offsetof(CpuState, reserved));
inline constexpr int32_t kStateSpefscr = int32_t(offsetof(CpuState, spefscr));

struct Block {
    ChainEntry chain;
    Entry entry = nullptr;
    uint32_t ea = 0;
    uint32_t pa = 0;
    std::vector<Insn> insns;  // the helper calls point into it: it stays where it is while the code can run
    uint32_t native = 0;
    uint32_t helpers = 0;

    uint32_t count() const {
        return uint32_t(insns.size());
    }
};

// ------------------------------------------------------------------------------------------- the assembler

enum class JOp : uint8_t { Add, Sub, And, Or, Xor, Shl, Shr, Sar, Mul, Rotl };
// signed (Lt, Gt, Le, Ge) and unsigned (Ltu, Gtu) compares, and equality
enum class JCond : uint8_t { Eq, Ne, Lt, Gt, Le, Ge, Ltu, Gtu };
enum class MemKind : uint8_t { Read, Write };

inline constexpr int kScratchRegisters = 6;
inline constexpr int kCachedRegisters = 7;

// The host-neutral interface the translator drives: six 32-bit scratch registers (0..5), labels, the CpuState's
// 32-bit fields, guest memory through the software TLB, and the step helper. A helper call clobbers every scratch
// register.
class BlockAssembler {
   public:
    virtual ~BlockAssembler() = default;

    // The CpuState fields (32-bit: GPRs, CR, CTR, LR) the block keeps in host registers, at most kCachedRegisters,
    // set before prologue(). load_state/store_state on them are register moves; they are loaded at the body, written
    // back before every step-helper call and at every exit, and reloaded after a step.
    virtual void set_cached(const std::vector<int32_t>& offsets) = 0;
    // saves the host's registers and loads the context, the state and the software TLB
    virtual void prologue() = 0;
    // writes the cached registers back, restores the host's registers and returns
    virtual void leave() = 0;
    // returns without writing the cached registers back (the state is already right: a step helper said to leave)
    virtual void leave_raw() = 0;

    virtual size_t new_label() = 0;
    virtual void bind(size_t label) = 0;
    virtual void jump(size_t label) = 0;

    // a 32-bit (or, for bytes = 1, an 8-bit zero-extended) field of the CpuState
    virtual void load_state(int d, int32_t offset, int bytes = 4) = 0;
    virtual void store_state(int32_t offset, int s) = 0;
    virtual void store_state_imm(int32_t offset, uint32_t value) = 0;
    // CpuState::cycles = Context::start_cycles + n
    virtual void set_cycles(uint32_t n) = 0;

    virtual void move_imm(int d, uint32_t value) = 0;
    virtual void move(int d, int s) = 0;
    virtual void op(JOp op, int d, int a, int b) = 0;
    virtual void op_imm(JOp op, int d, int a, uint32_t imm) = 0;
    virtual void not_(int d, int s) = 0;
    // d = (a cond b) ? 1 : 0
    virtual void set_if(JCond cond, int d, int a, int b) = 0;
    virtual void set_if_imm(JCond cond, int d, int a, uint32_t imm) = 0;
    virtual void branch_if(JCond cond, int a, int b, size_t label) = 0;
    virtual void branch_if_imm(JCond cond, int a, uint32_t imm, size_t label) = 0;
    // d = the leading zero count of s
    virtual void count_leading_zeros(int d, int s) = 0;
    // CR field `field` = LT GT EQ of `a` against `b` (a register, or with b < 0 the immediate), signed or unsigned,
    // and XER[SO]. Clobbers the scratch registers other than a and b.
    virtual void compare_field(unsigned field, int a, int b, uint32_t imm, bool is_signed) = 0;

    // d = the big-endian value of `size` bytes at effective `ea`, when the software TLB holds the page for reading and
    // the access stays in it; otherwise jumps to `slow`. d may be ea.
    virtual void mem_load(int d, int ea, int size, size_t slow) = 0;
    // stores `size` bytes of `value` at `ea` the same way; jumps to `slow` also when a reservation is held
    virtual void mem_store(int ea, int value, int size, size_t slow) = 0;

    // d = step(insn, pc, k)
    virtual void call_step(int d, const Insn* insn, uint32_t pc, uint32_t k) = 0;
    // d = fn(a, b, &CpuState::spefscr): an SPE operation that cannot fault (a = b for the unary ones)
    using SpeFunction = uint32_t (*)(uint32_t a, uint32_t b, uint32_t* spefscr);
    virtual void call_spe(int d, SpeFunction fn, int a, int b) = 0;

    // The block's end, `executed` instructions in, going on at `pc` (an immediate, or a scratch register): the pc and
    // the cycle count are stored, then the next block is entered directly when the table holds it for this pc and this
    // physical page and it ends within the budget; otherwise the block leaves. Clobbers every scratch register.
    virtual void chain_imm(uint32_t pc, uint32_t executed) = 0;
    virtual void chain_reg(int pc, uint32_t executed) = 0;
    // The running block's own address and physical page, for the shortcuts chain_imm takes: a jump to its own start
    // needs no lookup, and one within its own page needs no translation check (the page was checked on entry).
    virtual void set_block(uint32_t ea, uint32_t pa_page) = 0;
    // the body's start (bound after the prologue), from the start of the code, once flattened
    virtual void bind_body() = 0;
    virtual uint64_t body_offset() = 0;
};

// The host's assembler writing into `code` (an asmjit::CodeHolder), or null when the host has none.
std::unique_ptr<BlockAssembler> make_host_assembler(void* code_holder);

bool host_supported();

// Translates `insns` (consecutive from effective `ea`, physical `pa`) into `memory`. Null when the memory is full.
std::unique_ptr<Block> translate(sim::jit::CodeMemory& memory, uint32_t ea, uint32_t pa, std::vector<Insn> insns);

// ---------------------------------------------------------------------------------------------- the cache

struct JitStatistics {
    uint64_t translated = 0;
    uint64_t dropped = 0;
    uint64_t blocks_run = 0;
    uint64_t interpreted = 0;  // instructions run outside blocks (the limit or the decrementer near, a watch set)
    uint64_t resets = 0;       // the code memory filled and was emptied
};

class JitCache {
   public:
    explicit JitCache(size_t code_bytes = size_t(32) << 20) : memory_(code_bytes) {
    }

    // The block for effective `ea` at physical `pa`, or null if none is translated (or the one there was translated
    // for another physical page).
    Block* find(uint32_t ea, uint32_t pa) const {
        const ChainEntry* e = table_[(ea >> 2) & (kChainTableSize - 1)];
        if (e && e->ea == ea && e->pa_page == (pa >> 12)) {
            return e->block;
        }
        auto it = blocks_.find(ea);
        return (it != blocks_.end() && it->second->pa == pa) ? it->second.get() : nullptr;
    }
    // what chained exits look the next block up in
    ChainEntry* const* table() const {
        return table_.data();
    }
    // Translates and keeps a block; null if it cannot (the memory is full even after a reset).
    Block* insert(uint32_t ea, uint32_t pa, std::vector<Insn> insns);
    // The physical page was written: its blocks go (a running block leaves at its next helper, which sees the drop).
    void drop_page(uint32_t page);
    void clear();
    // frees the dropped blocks (between blocks only)
    void collect() {
        dropped_.clear();
    }

    JitStatistics stats;

   private:
    void unlink(const Block& block);

    sim::jit::CodeMemory memory_;
    std::vector<ChainEntry*> table_ = std::vector<ChainEntry*>(kChainTableSize, nullptr);
    std::unordered_map<uint32_t, std::unique_ptr<Block>> blocks_;          // by effective address
    std::unordered_map<uint32_t, std::vector<uint32_t>> by_page_;         // physical page -> effective addresses
    std::vector<std::unique_ptr<Block>> dropped_;
};

}  // namespace sim::e200::jit
