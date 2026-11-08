#include "gz60/adc.h"

#include <algorithm>
#include <cmath>

#include "hc08/machine.h"
#include "gz60/clocks.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr uint8_t kCoco = 0x80;
constexpr uint8_t kAien = 0x40;
constexpr uint8_t kAdco = 0x20;
constexpr uint8_t kChannelMask = 0x1F;
constexpr uint8_t kOff = 0x1F;
constexpr DeviceRegister kRegisters[] = {{"ADSCR", 0}, {"ADRH", 1}, {"ADRL", 2}, {"ADCLK", 3}};
}  // namespace

std::span<const DeviceRegister> Adc::registers() const {
    return kRegisters;
}

uint64_t Adc::conversion_ticks() const {
    const unsigned div = 1u << std::min(4, (adclk_ >> 5) & 7);
    const uint64_t clock_ticks =
        (adclk_ & 0x10) ? uint64_t(m->ticks_per_cycle()) * div : (m->cfg.tick_hz / kCrystalHz) * div;
    return clock_ticks * kClocksPerConversion;
}

uint64_t Adc::next_event() const {
    return paused_ ? kNever : done_at_;
}

void Adc::start(uint64_t now) {
    const int channel = adscr_ & kChannelMask;
    const double volts = sample ? sample(channel, now) : 0.0;
    const double clamped = std::clamp(volts, 0.0, kVref);
    pending_code_ = uint16_t(std::lround(clamped / kVref * 1023.0));
    done_at_ = now + conversion_ticks();
}

void Adc::store_result(uint16_t code10) {
    switch ((adclk_ >> 2) & 3) {
        case 0:  // 8-bit truncated
            adrh_ = 0;
            adrl_ = uint8_t(code10 >> 2);
            break;
        case 1:  // right-justified
            adrh_ = uint8_t(code10 >> 8);
            adrl_ = uint8_t(code10);
            break;
        case 2:  // left-justified
            adrh_ = uint8_t(code10 >> 2);
            adrl_ = uint8_t((code10 & 3) << 6);
            break;
        default: {  // left-justified signed
            const uint16_t s = uint16_t(code10 ^ 0x200);
            adrh_ = uint8_t(s >> 2);
            adrl_ = uint8_t((s & 3) << 6);
            break;
        }
    }
}

void Adc::sync(uint64_t now) {
    if (paused_ || now < done_at_) {
        return;
    }
    store_result(pending_code_);
    coco_ = true;
    conversions_++;
    done_at_ = kNever;
    if ((adscr_ & kAdco) && (adscr_ & kChannelMask) != kOff) {
        start(now);
    }
    irq.set(coco_ && (adscr_ & kAien), now);
}

void Adc::stop_mode(bool entering, uint64_t now) {
    if (entering && done_at_ != kNever) {
        paused_remaining_ = done_at_ > now ? done_at_ - now : 0;
        paused_ = true;
    } else if (!entering && paused_) {
        paused_ = false;
        done_at_ = now + paused_remaining_;
    }
}

void Adc::reset() {
    adscr_ = 0x1F;
    adclk_ = 0;
    adrh_ = adrl_ = 0;
    coco_ = false;
    done_at_ = kNever;
    paused_ = false;
    irq.set(false, 0);
}

uint8_t Adc::peek(uint16_t offset) {
    switch (offset) {
        case 0:
            return uint8_t((adscr_ & 0x7F) | (coco_ ? kCoco : 0));
        case 1:
            return adrh_;
        case 2:
            return adrl_;
        default:
            return adclk_;
    }
}

uint8_t Adc::rd(uint16_t offset) {
    const uint8_t value = peek(offset);
    const bool left = ((adclk_ >> 2) & 3) >= 2;
    if ((offset == 2 && !left) || (offset == 1 && left)) {
        coco_ = false;
        irq.set(false, m->now());
    }
    return value;
}

void Adc::wr(uint16_t offset, uint8_t value) {
    const uint64_t now = m->now();
    switch (offset) {
        case 0:
            adscr_ = uint8_t(value & 0x7F);
            coco_ = false;
            if ((adscr_ & kChannelMask) == kOff) {
                done_at_ = kNever;  // ADC powered off
            } else {
                start(now);
            }
            irq.set(false, now);
            break;
        case 3:
            adclk_ = uint8_t(value & 0xFC);
            break;
        default:
            break;
    }
}

void Adc::save(std::vector<uint8_t>& out) const {
    put_value(out, adscr_);
    put_value(out, adclk_);
    put_value(out, adrh_);
    put_value(out, adrl_);
    put_value(out, coco_);
    put_value(out, pending_code_);
    put_value(out, done_at_);
    put_value(out, paused_remaining_);
    put_value(out, paused_);
    put_value(out, conversions_);
}

void Adc::load(const uint8_t*& in) {
    get_value(in, adscr_);
    get_value(in, adclk_);
    get_value(in, adrh_);
    get_value(in, adrl_);
    get_value(in, coco_);
    get_value(in, pending_code_);
    get_value(in, done_at_);
    get_value(in, paused_remaining_);
    get_value(in, paused_);
    get_value(in, conversions_);
}

}  // namespace sim::sys
