// FlexCAN2, the MPC55xx CAN controller (MPC5554 RM, chapter 22): module disable, freeze and halt, soft reset, the
// bit timing, 64 message buffers with transmit (by lowest ID, or lowest buffer with CTRL[LBUF]) and receive (first
// empty matching buffer, else the last matching full one overrun) through the global, 14 and 15 masks, the buffer
// interrupt flags and masks, and the free-running timer for timestamps. Error counters and error states are not
// modelled (the bus here never errs): ECR and ESR read 0 but for ESR[IDLE].
//
// The controller is a CanNode on a CanBus. A transmission takes the frame's time on the wire at the programmed bit
// rate; the frame reaches the other nodes when it completes. A frame from the bus is received at the moment it is sent.
//
// Registers: MCR 0x00, CTRL 0x04, TIMER 0x08, RXGMASK 0x10, RX14MASK 0x14, RX15MASK 0x18, ECR 0x1C, ESR 0x20,
// IMRH 0x24, IMRL 0x28, IFRH 0x2C, IFRL 0x30, the buffers from 0x80 (16 bytes each: control/status, ID, data).
#pragma once

#include <array>
#include <cstdint>

#include "e200/machine.h"
#include "emu/can_bus.h"
#include "emu/irq_line.h"
#include "mpc55xx/registers.h"

namespace sim::mpc55xx {

class FlexCan final : public e200::BusDevice, public sys::CanNode {
   public:
    static constexpr int kBuffers = 64;

    // `clock_hz`: the system clock (CTRL[CLKSRC] = 1); `oscillator_hz`: the crystal (CLKSRC = 0).
    FlexCan(std::string name, uint64_t clock_hz, uint64_t oscillator_hz);

    // The interrupt lines: buffers 0..15 one each, 16..31 and 32..63 one per group, and the error and bus-off lines.
    std::array<sys::IrqLine, 16> buffer_irq;
    sys::IrqLine buffers_16_31_irq;
    sys::IrqLine buffers_32_63_irq;

    void attach(sys::CanBus& bus) {
        bus_ = &bus;
        bus.attach(*this);
    }

    // the bit rate the timing registers select (bits/s), 0 while the module is disabled or frozen
    uint32_t bit_rate() const;
    bool running() const;
    uint64_t frames_sent() const {
        return sent_;
    }
    uint64_t frames_received() const {
        return received_;
    }

    // CanNode
    bool receive(const sys::CanFrame& frame, const sys::CanNode& from, uint64_t now) override;

    // BusDevice
    uint32_t read(uint32_t offset, unsigned size) override;
    uint32_t peek(uint32_t offset, unsigned size) override;
    void write(uint32_t offset, unsigned size, uint32_t value) override;
    uint64_t next_event() const override {
        return tx_done_;
    }
    void sync(uint64_t now) override;
    void reset() override;
    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   private:
    static uint32_t mb(int n) {
        return 0x80 + 16 * uint32_t(n);
    }
    uint32_t code(int n) const {
        return (regs_.get32(mb(n)) >> 24) & 0xF;
    }
    int max_buffer() const {
        return int(regs_.get32(0x00) & 0x3F);
    }
    uint64_t flags() const {
        return (uint64_t(regs_.get32(0x2C)) << 32) | regs_.get32(0x30);
    }
    void set_flag(int n);
    uint64_t frame_ticks(const sys::CanFrame& frame) const;
    uint16_t timer_at(uint64_t now) const;
    void start_next_transmission(uint64_t now);
    void update_irqs(uint64_t now);
    void write_mcr(uint32_t value);

    uint64_t clock_hz_;
    uint64_t oscillator_hz_;
    RegisterFile regs_;
    sys::CanBus* bus_ = nullptr;
    int tx_buffer_ = -1;
    uint64_t tx_done_ = kNever;
    uint64_t sent_ = 0;
    uint64_t received_ = 0;
};

}  // namespace sim::mpc55xx
