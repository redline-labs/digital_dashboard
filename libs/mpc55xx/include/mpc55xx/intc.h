// The INTC, the MPC55xx interrupt controller (MPC5554 RM, chapter 10), in software vector mode: each source has a
// 4-bit priority (PSR); the controller raises the core's external input while a pending source's priority is above
// the current priority (CPR). Reading IACKR acknowledges: it returns the vector table address with the source's
// vector, pushes CPR onto the priority LIFO and raises CPR to the source's priority; writing EOIR pops it. The eight
// software sources are set and cleared through SSCIR.
//
// Registers: MCR 0x00 (HVEN, VTES), CPR 0x08, IACKR 0x10, EOIR 0x18, SSCIR0-7 0x20, PSR0-n 0x40 (one byte each).
// Hardware vector mode (MCR[HVEN]) is not modelled: it is logged as a model gap and runs as software mode.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

#include "e200/machine.h"
#include "emu/irq_line.h"

namespace sim::mpc55xx {

class Intc final : public e200::BusDevice, public sys::IrqSink {
   public:
    static constexpr int kSources = 308;  // MPC5554

    Intc() : BusDevice("intc") {
        reset();
    }

    // The line a peripheral raises for source `n`.
    sys::IrqLine line(int n) {
        return sys::IrqLine(this, n);
    }

    void set_line(int src, bool on) override;

    uint32_t read(uint32_t offset, unsigned size) override;
    uint32_t peek(uint32_t offset, unsigned size) override;
    void write(uint32_t offset, unsigned size, uint32_t value) override;
    void reset() override;
    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

    uint32_t current_priority() const {
        return cpr_;
    }
    bool pending(int src) const {
        return src >= 0 && src < kSources && (pending_.test(size_t(src)) || software(src));
    }

   private:
    bool software(int src) const {
        return src < 8 && (sscir_[size_t(src)] & 1);
    }
    // the highest-priority pending source above CPR (lowest number on a tie), or -1
    int best() const;
    void update();
    uint32_t register_value(uint32_t offset) const;

    std::bitset<kSources> pending_;
    std::array<uint8_t, kSources> psr_{};
    std::array<uint8_t, 8> sscir_{};
    uint32_t mcr_ = 0;
    uint32_t cpr_ = 15;
    uint32_t iackr_ = 0;  // VTBA (written), and the last vector acknowledged
    std::vector<uint32_t> lifo_;
};

}  // namespace sim::mpc55xx
