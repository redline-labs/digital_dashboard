#include "gz60/tim.h"

#include "hc08/machine.h"
#include "emu/model_gap.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr uint8_t kTof = 0x80;
constexpr uint8_t kToie = 0x40;
constexpr uint8_t kTstop = 0x20;
constexpr uint8_t kTrst = 0x10;
constexpr uint8_t kChf = 0x80;
constexpr uint8_t kChie = 0x40;
constexpr DeviceRegister kRegisters[] = {{"TSC", 0},   {"TCNTH", 1}, {"TCNTL", 2}, {"TMODH", 3},
                                         {"TMODL", 4}, {"TSC0", 5},  {"TCH0H", 6}, {"TCH0L", 7},
                                         {"TSC1", 8},  {"TCH1H", 9}, {"TCH1L", 10}};
model_gap::Once<16> channel_mode;
}  // namespace

std::span<const DeviceRegister> Tim::registers() const {
    return kRegisters;
}

bool Tim::running() const {
    return (tsc_ & kTstop) == 0 && !stopped_by_cpu_;
}

uint64_t Tim::ticks_per_count() const {
    const unsigned ps = tsc_ & 0x07;
    return uint64_t(m->ticks_per_cycle()) << (ps == 7 ? 0 : ps);
}

uint32_t Tim::period() const {
    return modulo_ == 0 ? 0x10000u : uint32_t(modulo_) + 1;
}

uint16_t Tim::count_at(uint64_t now) const {
    if (!running() || now < base_tick_) {
        return base_count_;
    }
    const uint64_t counts = (now - base_tick_) / ticks_per_count();
    const uint64_t position = uint64_t(base_count_) + counts;
    return uint16_t(position % period());
}

uint16_t Tim::counter() const {
    return count_at(m->now());
}

void Tim::rebase(uint64_t now) {
    base_count_ = count_at(now);
    // keep the sub-count phase: the base moves to the last whole count
    if (running() && now >= base_tick_) {
        const uint64_t tpc = ticks_per_count();
        base_tick_ = now - (now - base_tick_) % tpc;
    } else {
        base_tick_ = now;
    }

    if (running()) {
        const uint64_t to_wrap = uint64_t(period()) - base_count_;
        next_overflow_ = base_tick_ + to_wrap * ticks_per_count();
    } else {
        next_overflow_ = kNever;
    }
}

uint64_t Tim::next_event() const {
    if (!running()) {
        return kNever;
    }
    // only the overflow and enabled compares need a scheduled event; flags without interrupts are set lazily too,
    // but the overflow time is cheap to keep
    uint64_t next = next_overflow_;
    for (size_t k = 0; k < channels_; k++) {
        if ((csc_[k] & kChie) && (csc_[k] & 0x30) == 0x10 && !(csc_[k] & kChf)) {
            const uint16_t now_count = base_count_;
            const uint32_t distance = (uint32_t(ch_[k]) + period() - now_count) % period();
            const uint64_t at = base_tick_ + uint64_t(distance == 0 ? period() : distance) * ticks_per_count();
            next = std::min(next, at);
        }
    }
    return next;
}

void Tim::sync(uint64_t now) {
    if (!running()) {
        return;
    }

    // overflows since the last sync
    while (now >= next_overflow_) {
        tof_ = true;
        overflows_++;
        base_tick_ = next_overflow_;
        base_count_ = 0;
        next_overflow_ = base_tick_ + uint64_t(period()) * ticks_per_count();
    }

    // compares passed since the base (cheap: only enabled output-compare channels)
    for (size_t k = 0; k < channels_; k++) {
        if ((csc_[k] & 0x30) != 0x10 || (csc_[k] & kChf)) {
            continue;
        }
        const uint16_t count = count_at(now);
        const uint32_t from = base_count_;
        if ((ch_[k] >= from && ch_[k] <= count) || (count < from && (ch_[k] >= from || ch_[k] <= count))) {
            csc_[k] = uint8_t(csc_[k] | kChf);
        }
    }

    base_count_ = count_at(now);
    base_tick_ = now - (now - base_tick_) % ticks_per_count();
    update_irqs(now);
}

void Tim::update_irqs(uint64_t now) {
    overflow_irq.set(tof_ && (tsc_ & kToie), now);
    for (size_t k = 0; k < channels_; k++) {
        channel_irq[k].set((csc_[k] & kChf) && (csc_[k] & kChie), now);
    }
}

void Tim::bus_clock_changed(uint64_t now) {
    rebase(now);
}

void Tim::stop_mode(bool entering, uint64_t now) {
    rebase(now);
    stopped_by_cpu_ = entering;
    rebase(now);
}

void Tim::reset() {
    tsc_ = kTstop;
    modulo_ = 0xFFFF;
    base_count_ = 0;
    base_tick_ = m ? m->now() : 0;
    stopped_by_cpu_ = false;
    tof_ = false;
    tof_read_ = false;
    next_overflow_ = kNever;
    csc_.fill(0);
    ch_.fill(0xFFFF);
    chf_read_.fill(false);
    update_irqs(base_tick_);
}

uint8_t Tim::peek(uint16_t offset) {
    switch (offset) {
        case 0:
            return uint8_t((tsc_ & 0x7F) | (tof_ ? kTof : 0));
        case 1:
            return uint8_t(counter() >> 8);
        case 2:
            return uint8_t(counter());
        case 3:
            return uint8_t(modulo_ >> 8);
        case 4:
            return uint8_t(modulo_);
        default: {
            const size_t k = (size_t{offset} - 5) / 3;
            const size_t r = (size_t{offset} - 5) % 3;
            if (k >= channels_) {
                return 0;
            }
            return r == 0 ? csc_[k] : (r == 1 ? uint8_t(ch_[k] >> 8) : uint8_t(ch_[k]));
        }
    }
}

uint8_t Tim::rd(uint16_t offset) {
    const uint8_t value = peek(offset);
    if (offset == 0 && tof_) {
        tof_read_ = true;
    } else if (offset == 1) {
        latched_low_ = uint8_t(counter());
        low_latched_ = true;
    } else if (offset == 2 && low_latched_) {
        low_latched_ = false;
        return latched_low_;
    } else if (offset >= 5 && (offset - 5) % 3 == 0) {
        const size_t k = (size_t{offset} - 5) / 3;
        if (k < channels_ && (csc_[k] & kChf)) {
            chf_read_[k] = true;
        }
    }
    return value;
}

void Tim::wr(uint16_t offset, uint8_t value) {
    const uint64_t now = m->now();

    switch (offset) {
        case 0: {
            // TOF clears by reading it set, then writing 0 to it
            if (!(value & kTof) && tof_read_) {
                tof_ = false;
                tof_read_ = false;
            }
            const bool restart = (value & kTrst) != 0;
            rebase(now);
            tsc_ = uint8_t(value & 0x67);
            if (restart) {
                base_count_ = 0;
                base_tick_ = now;
            }
            rebase(now);
            break;
        }
        case 1:
        case 2:
            break;  // the counter is read-only
        case 3:
            rebase(now);
            modulo_ = uint16_t((uint16_t(value) << 8) | (modulo_ & 0xFF));
            rebase(now);
            break;
        case 4:
            rebase(now);
            modulo_ = uint16_t((modulo_ & 0xFF00) | value);
            if (base_count_ >= period()) {
                base_count_ = 0;
            }
            rebase(now);
            break;
        default: {
            const size_t k = (size_t{offset} - 5) / 3;
            const size_t r = (size_t{offset} - 5) % 3;
            if (k >= channels_) {
                return;
            }
            if (r == 0) {
                const bool clear = !(value & kChf) && chf_read_[k];
                csc_[k] = uint8_t((value & 0x7F) | (clear ? 0 : (csc_[k] & kChf)));
                if (clear) {
                    chf_read_[k] = false;
                }
                if ((value & 0x30) != 0 && (value & 0x30) != 0x10) {
                    model_gap::warn(channel_mode, k, "{}: channel {} mode ${:02x} (capture/PWM) not modelled",
                                    name(), k, value);
                }
            } else if (r == 1) {
                ch_[k] = uint16_t((uint16_t(value) << 8) | (ch_[k] & 0xFF));
            } else {
                ch_[k] = uint16_t((ch_[k] & 0xFF00) | value);
            }
            break;
        }
    }
    update_irqs(now);
}

void Tim::save(std::vector<uint8_t>& out) const {
    put_value(out, tsc_);
    put_value(out, modulo_);
    put_value(out, base_count_);
    put_value(out, base_tick_);
    put_value(out, stopped_by_cpu_);
    put_value(out, tof_);
    put_value(out, tof_read_);
    put_value(out, latched_low_);
    put_value(out, low_latched_);
    put_value(out, overflows_);
    put_value(out, next_overflow_);
    put_value(out, csc_);
    put_value(out, ch_);
    put_value(out, chf_read_);
}

void Tim::load(const uint8_t*& in) {
    get_value(in, tsc_);
    get_value(in, modulo_);
    get_value(in, base_count_);
    get_value(in, base_tick_);
    get_value(in, stopped_by_cpu_);
    get_value(in, tof_);
    get_value(in, tof_read_);
    get_value(in, latched_low_);
    get_value(in, low_latched_);
    get_value(in, overflows_);
    get_value(in, next_overflow_);
    get_value(in, csc_);
    get_value(in, ch_);
    get_value(in, chf_read_);
}

}  // namespace sim::sys
