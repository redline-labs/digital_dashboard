#include "gz60/stub.h"

#include "emu/model_gap.h"

namespace sim::sys {

namespace {
model_gap::Once<256> stub_write;
}

void StubDevice::wr(uint16_t offset, uint8_t value) {
    if (offset >= regs_.size()) {
        return;
    }
    if (value != 0) {
        const size_t key = (std::hash<std::string>{}(name()) * 31 + offset) & 0xFF;
        model_gap::warn(stub_write, key, "{}: write ${:02x} to register {} (module not modelled, stored)", name(),
                        value, offset);
    }
    regs_[offset] = value;
}

}  // namespace sim::sys
