#include "mpc55xx/registers.h"

#include "emu/model_gap.h"

namespace sim::mpc55xx {

namespace {
// one flag per (module, register) would need a key space per module; the site is keyed by the register's word
// offset modulo the table, and the message names the module, so a new register on a new module still shows
sim::model_gap::Once<4096> first_write;
}  // namespace

uint32_t StubModule::read(uint32_t offset, unsigned size) {
    for (const Fixed& f : fixed_) {
        if ((offset & ~3u) == f.offset) {
            RegisterFile word(4);
            word.set32(0, f.value);
            return word.get(offset & 3, size);
        }
    }
    return regs_.get(offset, size);
}

void StubModule::write(uint32_t offset, unsigned size, uint32_t value) {
    const size_t word = offset / 4;
    if (word < written_.size() && !written_[word]) {
        written_[word] = true;
        sim::model_gap::warn(first_write, (std::hash<std::string>{}(name()) + word) % 4096,
                             "{}: register 0x{:04x} written (0x{:x}), stored only", name(), offset & ~3u, value);
    }
    regs_.set(offset, size, value);
}

}  // namespace sim::mpc55xx
