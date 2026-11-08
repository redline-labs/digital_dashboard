#include "gz60/cop.h"

#include <spdlog/spdlog.h>

#include "hc08/machine.h"
#include "gz60/clocks.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

uint64_t Cop::timeout_ticks() const {
    const uint64_t cycles = (uint64_t(1) << 18) - (uint64_t(1) << 4);
    return cycles * (m->cfg.tick_hz / kCrystalHz);
}

void Cop::set_enabled(bool on) {
    enabled_ = on;
    deadline_ = on && m ? m->now() + timeout_ticks() : kNever;
}

uint64_t Cop::next_event() const {
    return enabled_ && !stopped_ ? deadline_ : kNever;
}

void Cop::sync(uint64_t now) {
    if (enabled_ && !stopped_ && now >= deadline_) {
        SPDLOG_WARN("COP timeout at t={:.6f} s (pc=${:04x}): reset", double(now) / double(m->cfg.tick_hz), m->cpu.pc);
        deadline_ = now + timeout_ticks();
        m->request_reset(ResetCause::Cop);
    }
}

void Cop::stop_mode(bool entering, uint64_t now) {
    if (entering) {
        remaining_ = deadline_ > now ? deadline_ - now : 0;
        stopped_ = true;
    } else {
        stopped_ = false;
        // leaving STOP restarts the COP count (the SIM's stop recovery clears it)
        deadline_ = now + timeout_ticks();
        (void)remaining_;
    }
}

void Cop::reset() {
    stopped_ = false;
    deadline_ = enabled_ && m ? m->now() + timeout_ticks() : kNever;
}

uint8_t Cop::rd(uint16_t offset) {
    (void)offset;
    return m->mem.bytes[0xFFFF];
}

void Cop::wr(uint16_t offset, uint8_t value) {
    (void)offset;
    (void)value;
    feeds_++;
    if (enabled_) {
        deadline_ = m->now() + timeout_ticks();
    }
}

void Cop::save(std::vector<uint8_t>& out) const {
    put_value(out, enabled_);
    put_value(out, stopped_);
    put_value(out, deadline_);
    put_value(out, remaining_);
    put_value(out, feeds_);
}

void Cop::load(const uint8_t*& in) {
    get_value(in, enabled_);
    get_value(in, stopped_);
    get_value(in, deadline_);
    get_value(in, remaining_);
    get_value(in, feeds_);
}

}  // namespace sim::sys
