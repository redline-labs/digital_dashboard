// The Block engine's decode cache: each instruction is decoded once, the first time it runs, and kept per 256-byte
// page. The engine runs the cached instructions through the same run_core/execute_insn as the interpreter, so the
// two cannot differ in semantics, only in where the Insn comes from.
//
// Invalidation: the machine reports every write to a page that holds decoded code (has_code). The page and the one
// before it are dropped (an instruction is at most 4 bytes, so one that starts in the previous page may reach into
// this one). Code runs from FLASH-1, from FLASH-2 (the configuration's compiled logic) and in principle from RAM; flash
// is only written by the flash controller's program/erase, so in practice the cache is dropped only when PDM Manager's
// download (or a test) rewrites the configuration.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>

#include "hc08/cpu.h"
#include "hc08/isa.h"

namespace sim {

class DecodeCache {
   public:
    template <class Fetch>
    [[gnu::always_inline]] const Insn& get(uint16_t pc, Fetch&& fetch) {
        std::unique_ptr<Page>& page = pages_[pc >> 8];
        if (!page) {
            page = std::make_unique<Page>();
        }

        const uint8_t offset = uint8_t(pc);
        if (!page->valid.test(offset)) {
            page->insn[offset] = decode(pc, fetch);
            page->valid.set(offset);
            mark(pc >> 8);
            if (offset > 0xFC) {
                mark(((pc >> 8) + 1) & 0xFF);
            }
        }

        return page->insn[offset];
    }

    // True if the 256-byte page holds (part of) a decoded instruction.
    [[gnu::always_inline]] bool has_code(uint16_t addr) const {
        return code_pages_[addr >> 8] != 0;
    }

    // One byte per page, 1 where decoded code is: the JIT's generated stores read it.
    const uint8_t* code_page_flags() const {
        return code_pages_.data();
    }

    // Told every page dropped (the JIT drops its blocks there).
    std::function<void(unsigned page)> on_drop;

    // A byte in `addr`'s page changed: drop that page's and the previous page's instructions.
    void invalidate(uint16_t addr) {
        const unsigned page = addr >> 8;
        drop(page);
        drop((page + 0xFF) & 0xFF);
        invalidations_++;
    }

    void clear() {
        for (unsigned page = 0; page < 256; page++) {
            drop(page);
        }
    }

    uint64_t invalidations() const {
        return invalidations_;
    }

   private:
    struct Page {
        std::array<Insn, 256> insn{};
        std::bitset<256> valid;
    };

    void mark(unsigned page) {
        code_pages_[page] = 1;
    }

    void drop(unsigned page) {
        if (pages_[page]) {
            pages_[page]->valid.reset();
        }
        code_pages_[page] = 0;
        if (on_drop) {
            on_drop(page);
        }
    }

    std::array<std::unique_ptr<Page>, 256> pages_;
    std::array<uint8_t, 256> code_pages_{};
    uint64_t invalidations_ = 0;
};

// The Block engine: run_core over the decode cache.
template <class Env>
inline Stop run_blocks(CpuState& s, Env& env, DecodeCache& cache) {
    return run_core(s, env, [&env, &cache](uint16_t pc) -> const Insn& {
        return cache.get(pc, [&env](uint16_t addr) { return env.fetch(addr); });
    });
}

}  // namespace sim
