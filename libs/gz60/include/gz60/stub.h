// A module the PDM does not use (SPI, SCI/ESCI, KBI, IRQ control): its registers are storage, and the first write of
// a non-zero value to each is logged as a model gap, so a firmware path that starts using one shows up.
#pragma once

#include <cstdint>
#include <vector>

#include "hc08/periph.h"

namespace sim::sys {

class StubDevice final : public Periph {
   public:
    StubDevice(std::string name, uint16_t size, std::vector<uint8_t> reset_values = {})
        : Periph(std::move(name)), regs_(size, 0), reset_(std::move(reset_values)) {
        reset();
    }

    void reset() override {
        for (size_t k = 0; k < regs_.size(); k++) {
            regs_[k] = k < reset_.size() ? reset_[k] : 0;
        }
    }

    void save(std::vector<uint8_t>& out) const override {
        out.insert(out.end(), regs_.begin(), regs_.end());
    }

    void load(const uint8_t*& in) override {
        for (uint8_t& r : regs_) {
            r = *in++;
        }
    }

   protected:
    uint8_t rd(uint16_t offset) override {
        return offset < regs_.size() ? regs_[offset] : 0;
    }
    void wr(uint16_t offset, uint8_t value) override;

   private:
    std::vector<uint8_t> regs_;
    std::vector<uint8_t> reset_;
};

}  // namespace sim::sys
