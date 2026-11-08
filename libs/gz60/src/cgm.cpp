#include "gz60/cgm.h"

#include <spdlog/spdlog.h>

#include "hc08/machine.h"
#include "gz60/clocks.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr uint8_t kPllOn = 0x20;
constexpr uint8_t kBcs = 0x10;
constexpr uint8_t kLock = 0x40;
constexpr DeviceRegister kRegisters[] = {{"PCTL", 0}, {"PBWC", 1}, {"PMSH", 2}, {"PMSL", 3}, {"PMRS", 4}};
}  // namespace

std::span<const DeviceRegister> Cgm::registers() const {
    return kRegisters;
}

uint64_t Cgm::bus_hz() const {
    const uint8_t pctl = regs_[kPctl];
    if ((pctl & kBcs) == 0 || (pctl & kPllOn) == 0) {
        return kCrystalHz / 4;
    }

    // VPR1:0 (E) only selects the VCO's range: the frequency is N * CGMXCLK
    const uint64_t n = (uint64_t(regs_[kPmsh] & 0x0F) << 8) | regs_[kPmsl];
    return (n == 0 ? 1 : n) * kCrystalHz / 4;
}

void Cgm::reset() {
    regs_ = {0x00, 0x00, 0x00, 0x40, 0x40};  // the data sheet's reset values (N = 64, L = 64)
    apply();
}

void Cgm::set_bootloader_state() {
    regs_[kPmsh] = 0;
    regs_[kPmsl] = 8;
    regs_[kPmrs] = 0x70;
    regs_[kPbwc] = 0x80;
    regs_[kPctl] = kPllOn | kBcs | 0x02;
    apply();
}

void Cgm::apply() {
    if (m) {
        const uint64_t hz = bus_hz();
        if (m->cfg.tick_hz % hz != 0) {
            SPDLOG_WARN("cgm: bus clock {} Hz does not divide the {} Hz tick: rounded", hz, m->cfg.tick_hz);
        }
        m->set_bus_hz(hz);
    }
}

uint8_t Cgm::rd(uint16_t offset) {
    if (offset == kPbwc) {
        return uint8_t((regs_[kPbwc] & ~kLock) | ((regs_[kPctl] & kPllOn) ? kLock : 0));
    }
    return offset < regs_.size() ? regs_[offset] : 0;
}

void Cgm::wr(uint16_t offset, uint8_t value) {
    if (offset >= regs_.size()) {
        return;
    }
    if (offset == kPbwc) {
        regs_[kPbwc] = uint8_t(value & ~kLock);
        return;
    }
    regs_[offset] = value;
    if (offset == kPctl) {
        apply();
    }
}

void Cgm::save(std::vector<uint8_t>& out) const {
    put_value(out, regs_);
}

void Cgm::load(const uint8_t*& in) {
    get_value(in, regs_);
}

}  // namespace sim::sys
