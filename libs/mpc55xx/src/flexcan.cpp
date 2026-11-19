#include "mpc55xx/flexcan.h"

#include <algorithm>

namespace sim::mpc55xx {

namespace {

constexpr uint32_t kMcr = 0x00, kCtrl = 0x04, kTimer = 0x08, kRxgMask = 0x10, kRx14Mask = 0x14, kRx15Mask = 0x18,
                   kEcr = 0x1C, kEsr = 0x20, kImrh = 0x24, kImrl = 0x28, kIfrh = 0x2C, kIfrl = 0x30;

// MCR: the bits that are stored, and the status bits composed on read
constexpr uint32_t kMdis = 0x80000000, kFrz = 0x40000000, kHalt = 0x10000000, kNotRdy = 0x08000000,
                   kSoftRst = 0x02000000, kFrzAck = 0x01000000, kSupv = 0x00800000, kMdisAck = 0x00100000;
constexpr uint32_t kMcrStored = 0xF0EF00FF & ~(kNotRdy | kSoftRst | kFrzAck | kMdisAck);
constexpr uint32_t kMcrReset = kMdis | kFrz | kHalt | kSupv | 0x0F;

constexpr uint32_t kCtrlLbuf = 0x10;
constexpr uint32_t kCtrlClkSrc = 0x2000;

// buffer control/status
constexpr uint32_t kSrr = 0x00400000, kIde = 0x00200000, kRtr = 0x00100000;
constexpr uint32_t kCodeTxInactive = 0x8, kCodeTxData = 0xC;
constexpr uint32_t kCodeRxEmpty = 0x4, kCodeRxFull = 0x2, kCodeRxOverrun = 0x6;

uint32_t id_word(const sys::CanFrame& f) {
    return f.isExtended ? (f.id & 0x1FFFFFFF) : ((f.id & 0x7FF) << 18);
}

}  // namespace

FlexCan::FlexCan(std::string name, uint64_t clock_hz, uint64_t oscillator_hz)
    : BusDevice(std::move(name), sys::Timing::Timed), clock_hz_(clock_hz), oscillator_hz_(oscillator_hz),
      regs_(0x80 + 16 * kBuffers) {
    reset();
}

void FlexCan::reset() {
    regs_.clear();
    regs_.set32(kMcr, kMcrReset);
    regs_.set32(kRxgMask, 0x1FFFFFFF);
    regs_.set32(kRx14Mask, 0x1FFFFFFF);
    regs_.set32(kRx15Mask, 0x1FFFFFFF);
    tx_buffer_ = -1;
    tx_done_ = kNever;
}

bool FlexCan::running() const {
    const uint32_t mcr = regs_.get32(kMcr);
    return !(mcr & kMdis) && !((mcr & kFrz) && (mcr & kHalt));
}

uint32_t FlexCan::bit_rate() const {
    if (!running()) {
        return 0;
    }
    const uint32_t ctrl = regs_.get32(kCtrl);
    const uint64_t clock = (ctrl & kCtrlClkSrc) ? clock_hz_ : oscillator_hz_;
    const uint32_t presdiv = (ctrl >> 24) + 1;
    const uint32_t quanta = 1 + ((ctrl & 0x7) + 1) + (((ctrl >> 19) & 0x7) + 1) + (((ctrl >> 16) & 0x7) + 1);
    return uint32_t(clock / (uint64_t(presdiv) * quanta));
}

uint64_t FlexCan::frame_ticks(const sys::CanFrame& frame) const {
    const uint32_t rate = bit_rate();
    if (rate == 0 || !m) {
        return 1;
    }
    // the frame without stuff bits: SOF, arbitration, control, data, CRC, ACK, EOF and the intermission
    const uint64_t bits = (frame.isExtended ? 67u : 47u) + 8u * sys::classic_payload_bytes(frame);
    return std::max<uint64_t>(1, bits * m->cfg.core_hz / rate);
}

uint16_t FlexCan::timer_at(uint64_t now) const {
    const uint32_t rate = bit_rate();
    if (rate == 0 || !m) {
        return 0;
    }
    return uint16_t(now * rate / m->cfg.core_hz);
}

void FlexCan::set_flag(int n) {
    if (n < 32) {
        regs_.set32(kIfrl, regs_.get32(kIfrl) | (1u << n));
    } else {
        regs_.set32(kIfrh, regs_.get32(kIfrh) | (1u << (n - 32)));
    }
}

void FlexCan::update_irqs(uint64_t now) {
    const uint64_t pending = flags() & ((uint64_t(regs_.get32(kImrh)) << 32) | regs_.get32(kImrl));
    for (int n = 0; n < 16; ++n) {
        buffer_irq[size_t(n)].set(((pending >> n) & 1) != 0, now);
    }
    buffers_16_31_irq.set(((pending >> 16) & 0xFFFF) != 0, now);
    buffers_32_63_irq.set((pending >> 32) != 0, now);
}

void FlexCan::start_next_transmission(uint64_t now) {
    if (tx_buffer_ >= 0 || !running() || bit_rate() == 0) {
        return;
    }
    const bool by_buffer = (regs_.get32(kCtrl) & kCtrlLbuf) != 0;
    int best = -1;
    uint32_t best_key = 0;
    for (int n = 0; n <= std::min(max_buffer(), kBuffers - 1); ++n) {
        if (code(n) != kCodeTxData) {
            continue;
        }
        // arbitration: the identifier as it goes on the wire (a standard frame's 11 bits first)
        const uint32_t key = regs_.get32(mb(n) + 4) & 0x1FFFFFFF;
        if (best < 0 || (!by_buffer && key < best_key)) {
            best = n;
            best_key = key;
        }
    }
    if (best < 0) {
        return;
    }
    const uint32_t cs = regs_.get32(mb(best));
    sys::CanFrame frame{};
    frame.isExtended = (cs & kIde) != 0;
    frame.isRTR = (cs & kRtr) != 0;
    frame.len = uint8_t(std::min<uint32_t>((cs >> 16) & 0xF, 8));
    tx_buffer_ = best;
    tx_done_ = now + frame_ticks(frame);
    if (m) {
        m->reschedule(tx_done_);
    }
}

void FlexCan::sync(uint64_t now) {
    while (tx_buffer_ >= 0 && now >= tx_done_) {
        const int n = tx_buffer_;
        const uint64_t done = tx_done_;
        const uint32_t cs = regs_.get32(mb(n));
        const uint32_t id = regs_.get32(mb(n) + 4);
        sys::CanFrame frame{};
        frame.isExtended = (cs & kIde) != 0;
        frame.isRTR = (cs & kRtr) != 0;
        frame.id = frame.isExtended ? (id & 0x1FFFFFFF) : ((id >> 18) & 0x7FF);
        frame.len = uint8_t(std::min<uint32_t>((cs >> 16) & 0xF, 8));
        for (uint32_t k = 0; k < 8; ++k) {
            frame.data[k] = uint8_t(regs_.get(mb(n) + 8 + k, 1));
        }
        tx_buffer_ = -1;
        tx_done_ = kNever;
        if (code(n) == kCodeTxData) {
            regs_.set32(mb(n), (cs & 0x007F0000) | (kCodeTxInactive << 24) | timer_at(done));
            set_flag(n);
            sent_++;
            if (bus_) {
                bus_->send(*this, frame, done);
            }
        }
        start_next_transmission(done);
    }
    update_irqs(now);
}

bool FlexCan::receive(const sys::CanFrame& frame, const sys::CanNode&, uint64_t now) {
    if (!running() || frame.isFD || frame.isError) {
        return false;
    }
    const uint32_t want = id_word(frame);
    int target = -1;
    int last_full = -1;
    for (int n = 0; n <= std::min(max_buffer(), kBuffers - 1); ++n) {
        const uint32_t c = code(n);
        if (c != kCodeRxEmpty && c != kCodeRxFull && c != kCodeRxOverrun) {
            continue;
        }
        const uint32_t cs = regs_.get32(mb(n));
        if (((cs & kIde) != 0) != frame.isExtended) {
            continue;
        }
        const uint32_t mask = n == 14 ? regs_.get32(kRx14Mask) : n == 15 ? regs_.get32(kRx15Mask)
                                                                          : regs_.get32(kRxgMask);
        if (((regs_.get32(mb(n) + 4) ^ want) & mask & 0x1FFFFFFF) != 0) {
            continue;
        }
        if (c == kCodeRxEmpty) {
            target = n;
            break;
        }
        last_full = n;
    }
    // the controller acknowledges every frame it sees while it runs, matched or not
    const bool overrun = target < 0 && last_full >= 0;
    if (overrun) {
        target = last_full;
    }
    if (target >= 0) {
        const uint32_t new_code = overrun ? kCodeRxOverrun : kCodeRxFull;
        const uint32_t cs = (new_code << 24) | (frame.isExtended ? (kSrr | kIde) : 0) | (frame.isRTR ? kRtr : 0) |
                            (uint32_t(std::min<uint8_t>(frame.len, 8)) << 16) | timer_at(now);
        regs_.set32(mb(target), cs);
        regs_.set32(mb(target) + 4, (regs_.get32(mb(target) + 4) & 0xE0000000) | want);
        for (uint32_t k = 0; k < 8; ++k) {
            regs_.set(mb(target) + 8 + k, 1, k < frame.len ? frame.data[k] : 0);
        }
        set_flag(target);
        received_++;
        update_irqs(now);
    }
    return true;
}

uint32_t FlexCan::peek(uint32_t offset, unsigned size) {
    if ((offset & ~3u) == kMcr) {
        uint32_t mcr = regs_.get32(kMcr);
        mcr &= ~(kNotRdy | kFrzAck | kMdisAck);
        if (mcr & kMdis) {
            mcr |= kMdisAck | kNotRdy;
        } else if ((mcr & kFrz) && (mcr & kHalt)) {
            mcr |= kFrzAck | kNotRdy;
        }
        RegisterFile word(4);
        word.set32(0, mcr);
        return word.get(offset & 3, size);
    }
    if ((offset & ~3u) == kTimer) {
        RegisterFile word(4);
        word.set32(0, m ? timer_at(m->now()) : 0);
        return word.get(offset & 3, size);
    }
    if ((offset & ~3u) == kEsr) {
        RegisterFile word(4);
        word.set32(0, running() ? 0x00000080u : 0u);  // IDLE
        return word.get(offset & 3, size);
    }
    if ((offset & ~3u) == kEcr) {
        return 0;
    }
    return regs_.get(offset, size);
}

uint32_t FlexCan::read(uint32_t offset, unsigned size) {
    return peek(offset, size);
}

void FlexCan::write_mcr(uint32_t value) {
    if (value & kSoftRst) {
        // the soft reset: every register but the masks and the buffers, and it completes at once
        const uint32_t keep_ctrl = 0;
        regs_.set32(kMcr, (regs_.get32(kMcr) & (kMdis)) | (kMcrReset & ~kMdis));
        regs_.set32(kCtrl, keep_ctrl);
        regs_.set32(kImrh, 0);
        regs_.set32(kImrl, 0);
        regs_.set32(kIfrh, 0);
        regs_.set32(kIfrl, 0);
        tx_buffer_ = -1;
        tx_done_ = kNever;
        return;
    }
    regs_.set32(kMcr, value & kMcrStored);
}

void FlexCan::write(uint32_t offset, unsigned size, uint32_t value) {
    const uint64_t now = m ? m->now() : 0;
    const uint32_t reg = offset & ~3u;
    if (reg == kMcr) {
        RegisterFile word(4);
        word.set32(0, regs_.get32(kMcr));
        word.set(offset & 3, size, value);
        write_mcr(word.get32(0));
        if (!running()) {
            tx_buffer_ = -1;
            tx_done_ = kNever;
        }
        start_next_transmission(now);
        update_irqs(now);
        return;
    }
    if (reg == kIfrh || reg == kIfrl) {
        // write 1 to clear
        RegisterFile word(4);
        word.set(offset & 3, size, value);
        regs_.set32(reg, regs_.get32(reg) & ~word.get32(0));
        update_irqs(now);
        return;
    }
    if (reg == kTimer || reg == kEcr || reg == kEsr) {
        return;
    }
    regs_.set(offset, size, value);
    if (offset >= 0x80) {
        start_next_transmission(now);
    }
    update_irqs(now);
}

}  // namespace sim::mpc55xx
