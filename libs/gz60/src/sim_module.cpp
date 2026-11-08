#include "gz60/sim_module.h"

#include "hc08/machine.h"
#include "gz60/cop.h"
#include "emu/model_gap.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
model_gap::Once<64> break_register;
}

void SimModule::reset() {
    regs_.fill(0);
    config1_written_ = false;
    config2_written_ = false;
}

uint8_t SimModule::interrupt_status(int index) const {
    // IFn is source n + 2 (n = 1: CGM); INT1 bits 7..2 = IF6..IF1, INT2 = IF14..IF7, INT3 = IF22..IF15
    const uint32_t lines = m ? m->irq_lines() : 0;
    uint8_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
        int flag;
        if (index == 0) {
            if (bit < 2) {
                continue;
            }
            flag = bit - 1;
        } else {
            flag = (index == 1 ? 7 : 15) + bit;
        }
        if ((lines >> (flag + 2)) & 1) {
            value = uint8_t(value | (1u << bit));
        }
    }
    return value;
}

uint8_t SimModule::peek(uint16_t offset) {
    switch (offset) {
        case 0x01:
            return srsr_;
        case 0x04:
        case 0x05:
        case 0x06:
            return interrupt_status(offset - 4);
        case kConfig1:
            return config1_;
        case kConfig2:
            return config2_;
        default:
            return offset < regs_.size() ? regs_[offset] : 0;
    }
}

uint8_t SimModule::rd(uint16_t offset) {
    const uint8_t value = peek(offset);
    if (offset == 0x01) {
        srsr_ = 0;  // reading SRSR clears it
    }
    return value;
}

void SimModule::wr(uint16_t offset, uint8_t value) {
    if (offset == kConfig1) {
        if (!config1_written_) {
            config1_ = value;
            config1_written_ = true;
            if (cop) {
                cop->set_enabled((value & 0x01) == 0);  // COPD
            }
        }
        return;
    }
    if (offset == kConfig2) {
        if (!config2_written_) {
            config2_ = value;
            config2_written_ = true;
        }
        return;
    }
    if (offset == 0x01 || (offset >= 0x04 && offset <= 0x06)) {
        return;  // read-only
    }
    if (offset < regs_.size()) {
        if (value != 0) {
            model_gap::warn(break_register, offset, "sim: write ${:02x} to $FE{:02x} (break/status), stored only",
                            value, offset);
        }
        regs_[offset] = value;
    }
}

void SimModule::save(std::vector<uint8_t>& out) const {
    put_value(out, srsr_);
    put_value(out, regs_);
    put_value(out, config1_);
    put_value(out, config2_);
    put_value(out, config1_written_);
    put_value(out, config2_written_);
}

void SimModule::load(const uint8_t*& in) {
    get_value(in, srsr_);
    get_value(in, regs_);
    get_value(in, config1_);
    get_value(in, config2_);
    get_value(in, config1_written_);
    get_value(in, config2_written_);
}

}  // namespace sim::sys
