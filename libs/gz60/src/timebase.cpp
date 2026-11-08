#include "gz60/timebase.h"

#include "hc08/machine.h"
#include "gz60/clocks.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr uint8_t kTbif = 0x80;
constexpr uint8_t kTack = 0x08;
constexpr uint8_t kTbie = 0x04;
constexpr uint8_t kTbon = 0x02;
constexpr uint32_t kDividers[8] = {32768, 8192, 2048, 128, 64, 32, 16, 8};
}  // namespace

uint64_t Timebase::period_ticks() const {
    return uint64_t(kDividers[(tbcr_ >> 4) & 7]) * (m->cfg.tick_hz / kCrystalHz);
}

uint64_t Timebase::next_event() const {
    return (tbcr_ & kTbon) ? next_ : kNever;
}

void Timebase::sync(uint64_t now) {
    if (!(tbcr_ & kTbon)) {
        return;
    }
    while (now >= next_) {
        tbif_ = true;
        next_ += period_ticks();
    }
    irq.set(tbif_ && (tbcr_ & kTbie), now);
}

void Timebase::reset() {
    tbcr_ = 0;
    tbif_ = false;
    next_ = kNever;
    irq.set(false, 0);
}

uint8_t Timebase::peek(uint16_t offset) {
    (void)offset;
    return uint8_t((tbcr_ & 0x76) | (tbif_ ? kTbif : 0));
}

uint8_t Timebase::rd(uint16_t offset) {
    return peek(offset);
}

void Timebase::wr(uint16_t offset, uint8_t value) {
    (void)offset;
    const uint64_t now = m->now();
    const bool was_on = (tbcr_ & kTbon) != 0;
    tbcr_ = uint8_t(value & 0x76);
    if (value & kTack) {
        tbif_ = false;
    }
    if (!was_on && (tbcr_ & kTbon)) {
        next_ = now + period_ticks();
    }
    if (!(tbcr_ & kTbon)) {
        next_ = kNever;
    }
    irq.set(tbif_ && (tbcr_ & kTbie), now);
}

void Timebase::save(std::vector<uint8_t>& out) const {
    put_value(out, tbcr_);
    put_value(out, tbif_);
    put_value(out, next_);
}

void Timebase::load(const uint8_t*& in) {
    get_value(in, tbcr_);
    get_value(in, tbif_);
    get_value(in, next_);
}

}  // namespace sim::sys
