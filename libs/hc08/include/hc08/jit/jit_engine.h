// The Jit engine: the decode cache's blocks run as host code, translated the first
// time a block runs. The loop is run_core's, block by block:
//
//   - before each block: the cycle limit, the interrupt check (with the CLI/TAP delay), the pc watches
//   - a block runs only when its whole cycle cost fits under the limit (so no instruction inside it would have met the
//     limit) and no interrupt-mask delay is pending; otherwise one instruction is interpreted
//   - inside a block nothing can raise an interrupt or lower the limit except a helper access, after which the block
//     leaves (JitContext::exit)
//
// so the steps, stops and bus accesses are the interpreter's. A block's end chains into the next block in native code
// when these same checks pass there (jit/native_abi.h); the loop sees only the blocks it enters itself.
//
// The environment adds, to the interpreter's interface: jit_memory() (the 64 KB storage), jit_regions() (the region
// tags), jit_limit() (where cycle_limit() is kept) and jit_irq_lines() (non-zero exactly when irq_pending()).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "hc08/jit/code_memory.h"
#include "hc08/jit/native_abi.h"
#include "hc08/jit/translator.h"
#include "hc08/block_engine.h"
#include "hc08/cpu.h"

namespace sim::jit {

struct JitStatistics {
    uint64_t translated = 0;
    uint64_t dropped = 0;
    uint64_t blocks_run = 0;
    uint64_t interpreted = 0;  // instructions run outside blocks (limit near, CLI/TAP delay)
    uint64_t resets = 0;       // the code memory filled and was emptied
};

class JitCache {
   public:
    explicit JitCache(size_t code_bytes = size_t(32) << 20)
        : memory_(code_bytes), blocks_(65536), table_(65536, nullptr) {
    }

    // pc -> the block starting there (null: none): the chained exits' lookup
    ChainEntry* const* table() const {
        return table_.data();
    }

    // The block starting at `pc`, translated from the decode cache on first use.
    template <class Fetch>
    TranslatedBlock* get(uint16_t pc, DecodeCache& cache, Fetch&& fetch) {
        if (TranslatedBlock* known = blocks_[pc].get()) {
            return known;
        }

        std::vector<Insn> insns;
        uint16_t at = pc;
        while (insns.size() < size_t(kMaxBlockInstructions)) {
            const Insn& in = cache.get(at, fetch);
            insns.push_back(in);
            if (ends_block(in)) {
                break;
            }
            at = uint16_t(at + in.length);
            if ((at >> 8) != (pc >> 8)) {
                break;  // a block stays in its page (the unit the decode cache drops)
            }
        }
        return insert(pc, std::move(insns));
    }

    // The decode cache dropped a page: so do the blocks that start there (a running block leaves at its next check).
    void drop_page(unsigned page);
    void clear();
    // clear() and give the code memory back (between blocks only)
    void reset();
    // frees the dropped blocks (between blocks only: a running block's helper calls point into its instructions)
    void collect() {
        dropped_.clear();
    }

    JitContext* active = nullptr;
    JitStatistics stats;

    size_t blocks() const {
        return size_t(stats.translated - stats.dropped);
    }

   private:
    TranslatedBlock* insert(uint16_t pc, std::vector<Insn> insns);

    CodeMemory memory_;
    std::vector<std::unique_ptr<TranslatedBlock>> blocks_;  // by pc (65536)
    std::array<std::vector<uint16_t>, 256> by_page_;
    std::vector<std::unique_ptr<TranslatedBlock>> dropped_;
    std::vector<ChainEntry*> table_;
};

// ---------------------------------------------------------------------------------------------- the helpers

// A slow access leaves the block only if it changed what admitted the block: the limit came down (a device event
// moved into the quantum, a flash write, a reset request) or an interrupt line rose. Otherwise the admission still
// holds and the block goes on, as the interpreter would. (An invalidation sets `exit` through the drop listener.)
inline void note_access(JitContext* c, uint64_t limit_before, uint32_t lines_before) {
    if (*c->limit < limit_before || (*c->irq_lines & ~lines_before) != 0) {
        c->exit = 1;
    }
}

template <class Env>
uint32_t jit_read(JitContext* c, uint32_t addr, uint32_t pc) {
    (void)pc;
    const uint64_t limit = *c->limit;
    const uint32_t lines = *c->irq_lines;
    const uint32_t value = static_cast<Env*>(c->env)->read(uint16_t(addr));
    note_access(c, limit, lines);
    return value;
}

template <class Env>
void jit_write(JitContext* c, uint32_t addr, uint32_t value, uint32_t pc) {
    (void)pc;
    const uint64_t limit = *c->limit;
    const uint32_t lines = *c->irq_lines;
    static_cast<Env*>(c->env)->write(uint16_t(addr), uint8_t(value));
    note_access(c, limit, lines);
}

template <class Env>
uint32_t jit_step(JitContext* c, const Insn* insn, uint32_t remaining) {
    Env& env = *static_cast<Env*>(c->env);
    CpuState& s = *c->state;
    const uint16_t next = uint16_t(s.pc + insn->length);

    const Stop stop = execute_insn(s, env, *insn);
    if (stop.kind != StopKind::None) {
        c->stop = stop;
        c->has_stop = 1;
        return 0;
    }
    // continue only where run_core would run the next instruction without a check that could go otherwise
    if (c->exit || s.pc != next || s.irq_inhibit) {
        return 0;
    }
    if (!s.flag(ccr::kI) && env.irq_pending()) {
        return 0;
    }
    return s.cycles + remaining <= env.cycle_limit() ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------- the engine

template <class Env>
Stop run_jit(CpuState& s, Env& env, DecodeCache& cache, JitCache& jit) {
    JitContext ctx;
    ctx.state = &s;
    ctx.memory = env.jit_memory();
    ctx.regions = env.jit_regions();
    ctx.code_pages = cache.code_page_flags();
    ctx.env = &env;
    ctx.read = &jit_read<Env>;
    ctx.write = &jit_write<Env>;
    ctx.step = &jit_step<Env>;
    ctx.table = jit.table();
    ctx.limit = env.jit_limit();
    ctx.irq_lines = env.jit_irq_lines();

    struct Active {
        JitCache& jit;
        ~Active() {
            jit.active = nullptr;
            jit.collect();
        }
    } active{jit};
    jit.active = &ctx;

    const auto fetch = [&env](uint16_t addr) { return env.fetch(addr); };

    for (;;) {
        jit.collect();
        if (s.cycles >= env.cycle_limit()) {
            return Stop{StopKind::Limit, s.pc, 0};
        }

        const bool inhibit = s.irq_inhibit;
        s.irq_inhibit = false;
        if (!inhibit && !s.flag(ccr::kI) && env.irq_pending()) {
            enter_interrupt(s, env, env.irq_vector(), s.pc);
            continue;
        }
        if (env.watching() && int32_t(s.pc) != s.skip_watch_pc && env.pc_watched(s.pc)) {
            return Stop{StopKind::Watch, s.pc, 0};
        }
        s.skip_watch_pc = -1;

        TranslatedBlock* block = inhibit ? nullptr : jit.get(s.pc, cache, fetch);
        if (block == nullptr || s.cycles + block->cycles > env.cycle_limit()) {
            jit.stats.interpreted++;
            const Stop stop = execute_insn(s, env, cache.get(s.pc, fetch));
            if (stop.kind != StopKind::None) {
                return stop;
            }
            continue;
        }

        ctx.exit = 0;
        ctx.has_stop = 0;
        block->entry(&ctx);
        jit.stats.blocks_run++;
        if (ctx.has_stop) {
            return ctx.stop;
        }
    }
}

}  // namespace sim::jit
