#include "gz60/mscan.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "hc08/machine.h"
#include "gz60/clocks.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {

// offsets
constexpr uint16_t kCmcr0 = 0x00;
constexpr uint16_t kCmcr1 = 0x01;
constexpr uint16_t kCbtr0 = 0x02;
constexpr uint16_t kCbtr1 = 0x03;
constexpr uint16_t kCrflg = 0x04;
constexpr uint16_t kCrier = 0x05;
constexpr uint16_t kCtflg = 0x06;
constexpr uint16_t kCtcr = 0x07;
constexpr uint16_t kCidac = 0x08;
constexpr uint16_t kCidar = 0x10;
constexpr uint16_t kCidmr = 0x14;
constexpr uint16_t kRxBuffer = 0x40;
constexpr uint16_t kTxBuffer[3] = {0x50, 0x60, 0x70};
constexpr uint16_t kDlr = 0x0C;
constexpr uint16_t kTbpr = 0x0D;

// CMCR0
constexpr uint8_t kSynch = 0x10;
constexpr uint8_t kSlpak = 0x04;
constexpr uint8_t kSlprq = 0x02;
constexpr uint8_t kSftres = 0x01;
// CRFLG / CRIER
constexpr uint8_t kWupif = 0x80;
constexpr uint8_t kRxf = 0x01;
constexpr uint8_t kOvrif = 0x02;
constexpr uint8_t kErrorFlags = 0x7E;

constexpr DeviceRegister kRegisters[] = {
    {"CMCR0", 0x00},  {"CMCR1", 0x01},  {"CBTR0", 0x02},   {"CBTR1", 0x03},   {"CRFLG", 0x04},
    {"CRIER", 0x05},  {"CTFLG", 0x06},  {"CTCR", 0x07},    {"CIDAC", 0x08},   {"CRXERR", 0x0E},
    {"CTXERR", 0x0F}, {"CIDAR0", 0x10}, {"CIDAR1", 0x11},  {"CIDAR2", 0x12},  {"CIDAR3", 0x13},
    {"CIDMR0", 0x14}, {"CIDMR1", 0x15}, {"CIDMR2", 0x16},  {"CIDMR3", 0x17},  {"RXIDR0", 0x40},
    {"RXIDR1", 0x41}, {"RXDLR", 0x4C},  {"TX0IDR0", 0x50}, {"TX1IDR0", 0x60}, {"TX2IDR0", 0x70}};

}  // namespace

std::span<const DeviceRegister> Mscan::registers() const {
    return kRegisters;
}

void Mscan::attach(CanBus& bus) {
    bus_ = &bus;
    bus.attach(*this);
}

bool Mscan::in_soft_reset() const {
    return (regs_[kCmcr0] & kSftres) != 0;
}

bool Mscan::sleeping() const {
    return (regs_[kCmcr0] & kSlpak) != 0;
}

uint32_t Mscan::bit_rate() const {
    // CLKSRC: CGMOUT (twice the bus clock) or CGMXCLK / 2. The PDM uses CGMOUT: 16 MHz, so 1 Mbit/s at CBTR0=$01.
    const uint64_t clock = (regs_[kCmcr1] & 0x01) ? 2 * (m ? m->bus_hz() : 8'000'000) : kCrystalHz / 2;
    const uint32_t prescaler = (regs_[kCbtr0] & 0x3F) + 1;
    const uint32_t tseg1 = (regs_[kCbtr1] & 0x0F) + 1;
    const uint32_t tseg2 = ((regs_[kCbtr1] >> 4) & 0x07) + 1;
    return uint32_t(clock / (uint64_t(prescaler) * (1 + tseg1 + tseg2)));
}

uint64_t Mscan::frame_ticks(const CanFrame& frame) const {
    // SOF..EOF + intermission, without stuff bits: 47 bits + 8 per data byte (standard), 67 + 8n (extended)
    const uint64_t bits = (frame.isExtended ? 67 : 47) + 8ull * classic_payload_bytes(frame);
    const uint32_t rate = std::max<uint32_t>(1, bit_rate());
    return bits * m->cfg.tick_hz / rate;
}

std::array<uint8_t, 4> Mscan::encode_id(const CanFrame& frame) {
    std::array<uint8_t, 4> idr{};
    if (frame.isExtended) {
        const uint32_t id = frame.id & 0x1FFFFFFF;
        idr[0] = uint8_t(id >> 21);
        idr[1] = uint8_t(((id >> 13) & 0xE0) | 0x18 | ((id >> 15) & 0x07));
        idr[2] = uint8_t(id >> 7);
        idr[3] = uint8_t(((id & 0x7F) << 1) | (frame.isRTR ? 1 : 0));
    } else {
        const uint32_t id = frame.id & 0x7FF;
        idr[0] = uint8_t(id >> 3);
        idr[1] = uint8_t(((id & 7) << 5) | (frame.isRTR ? 0x10 : 0));
    }
    return idr;
}

void Mscan::decode_id(const uint8_t* idr, CanFrame& frame) {
    frame.isExtended = (idr[1] & 0x08) != 0;
    if (frame.isExtended) {
        frame.id = (uint32_t(idr[0]) << 21) | (uint32_t(idr[1] & 0xE0) << 13) | (uint32_t(idr[1] & 0x07) << 15) |
                   (uint32_t(idr[2]) << 7) | (uint32_t(idr[3]) >> 1);
        frame.isRTR = (idr[3] & 1) != 0;
    } else {
        frame.id = (uint32_t(idr[0]) << 3) | (idr[1] >> 5);
        frame.isRTR = (idr[1] & 0x10) != 0;
    }
}

bool Mscan::accepts(const CanFrame& frame, uint8_t& hit) const {
    const std::array<uint8_t, 4> idr = encode_id(frame);
    const uint8_t* ar = &regs_[kCidar];
    const uint8_t* mr = &regs_[kCidmr];
    const auto match = [&](size_t filter_byte, size_t id_byte) {
        return ((idr[id_byte] ^ ar[filter_byte]) & ~mr[filter_byte]) == 0;
    };

    switch ((regs_[kCidac] >> 4) & 3) {
        case 0: {  // one 32-bit filter (a standard frame compares IDR0/IDR1)
            hit = 0;
            const bool high = match(0, 0) && match(1, 1);
            return frame.isExtended ? high && match(2, 2) && match(3, 3) : high;
        }
        case 1:  // two 16-bit filters
            for (size_t f = 0; f < 2; f++) {
                if (match(2 * f, 0) && match(2 * f + 1, 1)) {
                    hit = uint8_t(f);
                    return true;
                }
            }
            return false;
        case 2:  // four 8-bit filters on IDR0
            for (size_t f = 0; f < 4; f++) {
                if (match(f, 0)) {
                    hit = uint8_t(f);
                    return true;
                }
            }
            return false;
        default:  // closed
            return false;
    }
}

void Mscan::deliver_to_foreground(const CanFrame& frame, uint8_t hit) {
    const std::array<uint8_t, 4> idr = encode_id(frame);
    for (size_t k = 0; k < 4; k++) {
        regs_[kRxBuffer + k] = idr[k];
    }
    for (size_t k = 0; k < 8; k++) {
        regs_[kRxBuffer + 4 + k] = frame.data[k];
    }
    regs_[kRxBuffer + kDlr] = std::min<uint8_t>(frame.len, 8);
    regs_[kCidac] = uint8_t((regs_[kCidac] & 0xFC) | (hit & 3));
    regs_[kCrflg] = uint8_t(regs_[kCrflg] | kRxf);
}

bool Mscan::receive(const CanFrame& frame, const CanNode& from, uint64_t now) {
    (void)from;
    if (m) {
        sync(now);
    }
    if (in_soft_reset()) {
        return false;
    }
    // A CAN FD frame is a form error to a classic controller; it never reaches a buffer.
    if (frame.isFD) {
        return false;
    }

    if (sleeping()) {
        // bus activity wakes the controller; the waking frame itself is lost
        regs_[kCmcr0] = uint8_t(regs_[kCmcr0] & ~(kSlpak | kSlprq));
        regs_[kCrflg] = uint8_t(regs_[kCrflg] | kWupif);
        update_irqs(now);
        dropped_++;
        return false;
    }

    uint8_t hit = 0;
    if (!accepts(frame, hit)) {
        return false;
    }

    received_++;
    if (!(regs_[kCrflg] & kRxf)) {
        deliver_to_foreground(frame, hit);
    } else if (!background_) {
        background_ = std::make_pair(frame, hit);
    } else {
        regs_[kCrflg] = uint8_t(regs_[kCrflg] | kOvrif);
        dropped_++;
    }
    update_irqs(now);
    return true;
}

void Mscan::start_next_transmission(uint64_t now) {
    if (tx_buffer_ >= 0 || queued_ == 0 || in_soft_reset() || sleeping()) {
        return;
    }

    // the queued buffer with the lowest local priority value; ties to the lowest buffer
    int best = -1;
    for (int b = 0; b < 3; b++) {
        if (!(queued_ & (1 << b))) {
            continue;
        }
        if (best < 0 || regs_[kTxBuffer[b] + kTbpr] < regs_[kTxBuffer[best] + kTbpr]) {
            best = b;
        }
    }

    CanFrame frame;
    decode_id(&regs_[kTxBuffer[best]], frame);
    frame.len = std::min<uint8_t>(regs_[kTxBuffer[best] + kDlr] & 0x0F, 8);
    tx_buffer_ = best;
    tx_done_ = now + frame_ticks(frame);
}

uint64_t Mscan::next_event() const {
    return tx_done_;
}

void Mscan::sync(uint64_t now) {
    while (tx_buffer_ >= 0 && now >= tx_done_) {
        const int b = tx_buffer_;
        const uint64_t done = tx_done_;
        CanFrame frame;
        decode_id(&regs_[kTxBuffer[b]], frame);
        frame.len = std::min<uint8_t>(regs_[kTxBuffer[b] + kDlr] & 0x0F, 8);
        for (size_t k = 0; k < 8; k++) {
            frame.data[k] = regs_[kTxBuffer[b] + 4 + k];
        }

        tx_buffer_ = -1;
        tx_done_ = kNever;
        queued_ = uint8_t(queued_ & ~(1 << b));
        regs_[kCtflg] = uint8_t(regs_[kCtflg] | (1 << b));  // TXE: the buffer is empty again
        sent_++;

        if (bus_) {
            bus_->send(*this, frame, done);
        }
        start_next_transmission(done);
    }
    update_irqs(now);
}

void Mscan::update_irqs(uint64_t now) {
    const uint8_t flags = regs_[kCrflg];
    const uint8_t enables = regs_[kCrier];
    wakeup_irq.set((flags & kWupif) && (enables & kWupif), now);
    error_irq.set((flags & enables & kErrorFlags) != 0, now);
    receive_irq.set((flags & kRxf) && (enables & kRxf), now);
    transmit_irq.set((regs_[kCtflg] & regs_[kCtcr] & 0x07) != 0, now);
}

void Mscan::reset() {
    regs_.fill(0);
    regs_[kCmcr0] = kSftres;
    regs_[kCtflg] = 0x07;  // all transmit buffers empty
    tx_buffer_ = -1;
    tx_done_ = kNever;
    queued_ = 0;
    background_.reset();
    update_irqs(0);
}

uint8_t Mscan::peek(uint16_t offset) {
    if (offset == kCmcr0) {
        return uint8_t((regs_[kCmcr0] & ~kSynch) | (in_soft_reset() ? 0 : kSynch));
    }
    if (offset == 0x0E || offset == 0x0F) {
        return 0;
    }
    return offset < regs_.size() ? regs_[offset] : 0;
}

uint8_t Mscan::rd(uint16_t offset) {
    return peek(offset);
}

void Mscan::wr(uint16_t offset, uint8_t value) {
    const uint64_t now = m->now();
    if (offset >= regs_.size()) {
        return;
    }

    switch (offset) {
        case kCmcr0: {
            const bool was_reset = in_soft_reset();
            uint8_t next = uint8_t(value & 0x2B);  // CSWAI, TLNKEN, SLPRQ, SFTRES
            if (next & kSftres) {
                // soft reset: the flags and the transmit state go back to reset
                regs_[kCrflg] = 0;
                regs_[kCtflg] = 0x07;
                regs_[kCtcr] = 0;
                tx_buffer_ = -1;
                tx_done_ = kNever;
                queued_ = 0;
                background_.reset();
            }
            if ((next & kSlprq) && !(next & kSftres) && tx_buffer_ < 0) {
                next = uint8_t(next | kSlpak);
            }
            regs_[kCmcr0] = next;
            if (was_reset && !in_soft_reset()) {
                SPDLOG_INFO("mscan: running at {} bit/s", bit_rate());
            }
            start_next_transmission(now);
            break;
        }
        case kCrflg:
            // write 1 to clear
            regs_[kCrflg] = uint8_t(regs_[kCrflg] & ~value);
            if ((value & kRxf) && background_) {
                deliver_to_foreground(background_->first, background_->second);
                background_.reset();
            }
            break;
        case kCtflg: {
            // writing 1 to TXEx clears it: the buffer is queued for transmission
            const uint8_t clear = uint8_t(value & 0x07 & regs_[kCtflg]);
            regs_[kCtflg] = uint8_t(regs_[kCtflg] & ~clear);
            queued_ = uint8_t(queued_ | clear);
            start_next_transmission(now);
            break;
        }
        case kCtcr: {
            regs_[kCtcr] = uint8_t(value & 0x77);
            // abort requests: a queued buffer not yet on the wire is given back
            for (int b = 0; b < 3; b++) {
                if ((value & (0x10 << b)) && (queued_ & (1 << b)) && tx_buffer_ != b) {
                    queued_ = uint8_t(queued_ & ~(1 << b));
                    regs_[kCtflg] = uint8_t(regs_[kCtflg] | (1 << b) | (0x10 << b));
                    regs_[kCtcr] = uint8_t(regs_[kCtcr] & ~(0x10 << b));
                }
            }
            break;
        }
        case kCidac:
            regs_[kCidac] = uint8_t((regs_[kCidac] & 0x03) | (value & 0x30));
            break;
        case 0x0E:
        case 0x0F:
            break;
        default:
            if (offset >= kRxBuffer && offset < kRxBuffer + 0x10) {
                break;  // the receive buffer is read-only
            }
            regs_[offset] = value;
            break;
    }
    update_irqs(now);
}

void Mscan::save(std::vector<uint8_t>& out) const {
    put_value(out, regs_);
    put_value(out, tx_buffer_);
    put_value(out, tx_done_);
    put_value(out, queued_);
    const bool has_background = background_.has_value();
    put_value(out, has_background);
    if (has_background) {
        put_value(out, background_->first);
        put_value(out, background_->second);
    }
    put_value(out, sent_);
    put_value(out, received_);
    put_value(out, dropped_);
}

void Mscan::load(const uint8_t*& in) {
    get_value(in, regs_);
    get_value(in, tx_buffer_);
    get_value(in, tx_done_);
    get_value(in, queued_);
    bool has_background = false;
    get_value(in, has_background);
    background_.reset();
    if (has_background) {
        CanFrame frame;
        uint8_t hit = 0;
        get_value(in, frame);
        get_value(in, hit);
        background_ = std::make_pair(frame, hit);
    }
    get_value(in, sent_);
    get_value(in, received_);
    get_value(in, dropped_);
}

}  // namespace sim::sys
