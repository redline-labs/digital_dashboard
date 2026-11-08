#include "hc08/assembler.h"

namespace sim {

std::vector<uint8_t> Hc08Asm::finish() const {
    std::vector<uint8_t> out = code_;
    for (const Fixup& f : fixups_) {
        const std::optional<size_t> target = positions_.at(f.label);
        if (!target) {
            throw std::logic_error("hc08 asm: unbound label");
        }
        // Relative to the byte after the offset; both are positions in code_, so the difference fits a ptrdiff_t.
        const auto rel = static_cast<std::ptrdiff_t>(*target) - static_cast<std::ptrdiff_t>(f.at + 1);
        if (rel < -128 || rel > 127) {
            throw std::logic_error("hc08 asm: branch out of reach");
        }
        out[f.at] = static_cast<uint8_t>(static_cast<int8_t>(rel));
    }
    return out;
}

}  // namespace sim
