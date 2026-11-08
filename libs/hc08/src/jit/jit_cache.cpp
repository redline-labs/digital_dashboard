#include <algorithm>

#include <spdlog/spdlog.h>

#include "hc08/jit/jit_engine.h"

namespace sim::jit {

TranslatedBlock* JitCache::insert(uint16_t pc, std::vector<Insn> insns) {
    std::unique_ptr<TranslatedBlock> block = translate(memory_, pc, insns);
    if (!block) {
        // the code memory is full: start over (between blocks, so nothing translated is running)
        reset();
        stats.resets++;
        SPDLOG_DEBUG("jit: code memory full, every block dropped");
        block = translate(memory_, pc, std::move(insns));
        if (!block) {
            return nullptr;
        }
    }
    stats.translated++;
    TranslatedBlock* raw = block.get();
    by_page_[pc >> 8].push_back(pc);
    table_[pc] = &raw->chain;
    blocks_[pc] = std::move(block);
    return raw;
}

void JitCache::drop_page(unsigned page) {
    std::vector<uint16_t>& pcs = by_page_[page & 0xFF];
    if (pcs.empty()) {
        return;
    }
    for (uint16_t pc : pcs) {
        if (blocks_[pc]) {
            table_[pc] = nullptr;
            dropped_.push_back(std::move(blocks_[pc]));
            stats.dropped++;
        }
    }
    pcs.clear();
    if (active) {
        active->exit = 1;
    }
}

void JitCache::clear() {
    for (auto& block : blocks_) {
        if (block) {
            dropped_.push_back(std::move(block));
            stats.dropped++;
        }
    }
    std::fill(table_.begin(), table_.end(), nullptr);
    for (auto& pcs : by_page_) {
        pcs.clear();
    }
    if (active) {
        active->exit = 1;
    }
}

void JitCache::reset() {
    clear();
    collect();
    memory_.reset();
}

}  // namespace sim::jit
