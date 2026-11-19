#include "mpc55xx/intc.h"

#include "emu/model_gap.h"

namespace sim::mpc55xx {

namespace {
sim::model_gap::Once<1> hardware_vector_mode;
}

void Intc::reset() {
    pending_.reset();
    psr_.fill(0);
    sscir_.fill(0);
    mcr_ = 0;
    cpr_ = 15;
    iackr_ = 0;
    lifo_.clear();
    if (m) {
        m->set_external_input(false);
    }
}

void Intc::set_line(int src, bool on) {
    if (src < 0 || src >= kSources) {
        return;
    }
    pending_.set(size_t(src), on);
    update();
}

int Intc::best() const {
    int found = -1;
    uint32_t level = cpr_;
    for (int s = 0; s < kSources; ++s) {
        if ((pending_.test(size_t(s)) || software(s)) && (psr_[size_t(s)] & 0xF) > level) {
            level = psr_[size_t(s)] & 0xF;
            found = s;
        }
    }
    return found;
}

void Intc::update() {
    if (m) {
        m->set_external_input(best() >= 0);
    }
}

uint32_t Intc::register_value(uint32_t offset) const {
    if (offset < 0x04) {
        return mcr_;
    }
    if (offset >= 0x08 && offset < 0x0C) {
        return cpr_;
    }
    if (offset >= 0x10 && offset < 0x14) {
        return iackr_;
    }
    return 0;
}

uint32_t Intc::peek(uint32_t offset, unsigned size) {
    if (offset >= 0x20 && offset < 0x28) {
        return sscir_[offset - 0x20] & 1;
    }
    if (offset >= 0x40 && offset < 0x40 + uint32_t(kSources)) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; ++i) {
            const uint32_t at = offset + i - 0x40;
            v = (v << 8) | (at < uint32_t(kSources) ? psr_[at] : 0);
        }
        return v;
    }
    const uint32_t word = register_value(offset & ~3u);
    const unsigned shift = 8 * (4 - size - (offset & 3));
    return size == 4 ? word : (word >> shift) & ((1u << (8 * size)) - 1);
}

uint32_t Intc::read(uint32_t offset, unsigned size) {
    if ((offset & ~3u) == 0x10) {
        // the acknowledge: the vector of the best source, and CPR rises to its priority
        const int s = best();
        if (s >= 0) {
            lifo_.push_back(cpr_);
            cpr_ = psr_[size_t(s)] & 0xF;
            const bool vtes = (mcr_ & 0x20) != 0;  // 8-byte entries: VTBA is bits 31..12
            const uint32_t field = vtes ? 0xFFFu : 0x7FFu;
            iackr_ = (iackr_ & ~field) | (uint32_t(s) << (vtes ? 3 : 2));
            update();
        }
    }
    return peek(offset, size);
}

void Intc::write(uint32_t offset, unsigned size, uint32_t value) {
    if (offset >= 0x20 && offset < 0x28) {
        for (unsigned i = 0; i < size; ++i) {
            const uint32_t n = offset + i - 0x20;
            const uint8_t b = uint8_t(value >> (8 * (size - 1 - i)));
            if (n < 8) {
                if (b & 0x02) {
                    sscir_[n] |= 1;  // SET
                }
                if (b & 0x01) {
                    sscir_[n] &= uint8_t(~1);  // CLR
                }
            }
        }
        update();
        return;
    }
    if (offset >= 0x40 && offset < 0x40 + uint32_t(kSources)) {
        for (unsigned i = 0; i < size; ++i) {
            const uint32_t at = offset + i - 0x40;
            if (at < uint32_t(kSources)) {
                psr_[at] = uint8_t(value >> (8 * (size - 1 - i))) & 0xF;
            }
        }
        update();
        return;
    }
    // the 32-bit registers (narrower writes are taken at the register's low end)
    switch (offset & ~3u) {
        case 0x00:
            mcr_ = value & 0x21;
            if (mcr_ & 0x01) {
                sim::model_gap::warn(hardware_vector_mode, "intc: hardware vector mode set; run as software mode");
            }
            break;
        case 0x08:
            cpr_ = value & 0xF;
            break;
        case 0x10:
            iackr_ = (iackr_ & 0x7FF) | (value & ~0x7FFu);  // VTBA (bit 11 is the vector's when VTES)
            break;
        case 0x18:
            // end of interrupt: the priority before the acknowledge comes back
            if (!lifo_.empty()) {
                cpr_ = lifo_.back();
                lifo_.pop_back();
            }
            break;
        default:
            break;
    }
    update();
}

}  // namespace sim::mpc55xx
