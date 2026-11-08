// MSCAN08, the GZ60's CAN controller ($0500..$057F; MC68HC908GZ60 data sheet, "MSCAN08 Controller"), as the PDM uses
// it: soft reset and initialisation, the bit timing (for the frame times), one 32/16/8-bit acceptance filter bank,
// a double-buffered receiver (foreground $0540 plus one background buffer), three transmit buffers with local
// priority, sleep with wake-up on bus activity, and the four interrupt sources. Error counters and error states are
// not modelled (the bus here never errs): CRXERR/CTXERR read 0.
//
// The controller is a CanNode on a wire CanBus (the same bus type as mk60e5's, so the two sims can share one later).
// A transmission takes the frame's time on the wire at the programmed bit rate; the frame reaches the other nodes when
// it completes. A frame from the bus is received at the moment it is sent (the sender has already spent its time).
//
// Registers (offsets from $0500): CMCR0 0, CMCR1 1, CBTR0 2, CBTR1 3, CRFLG 4, CRIER 5, CTFLG 6, CTCR 7, CIDAC 8,
// CRXERR $0E, CTXERR $0F, CIDAR0-3 $10, CIDMR0-3 $14, RX buffer $40, TX buffers $50/$60/$70 (IDR0-3, DSR0-7, DLR,
// TBPR).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "hc08/periph.h"
#include "emu/can_bus.h"
#include "emu/irq_line.h"

namespace sim::sys {

class Mscan final : public Periph, public CanNode {
   public:
    Mscan() : Periph("mscan", Timing::Timed) {
    }

    IrqLine wakeup_irq;
    IrqLine error_irq;
    IrqLine receive_irq;
    IrqLine transmit_irq;

    // The bus this controller is on (attached by the board).
    void attach(CanBus& bus);

    // the bit rate the timing registers select (bits/s), 0 in soft reset
    uint32_t bit_rate() const;

    bool sleeping() const;
    bool in_soft_reset() const;

    uint64_t frames_sent() const {
        return sent_;
    }
    uint64_t frames_received() const {
        return received_;
    }
    uint64_t frames_dropped() const {
        return dropped_;
    }

    // The MSCAN's IDR bytes for a frame, and back.
    static std::array<uint8_t, 4> encode_id(const CanFrame& frame);
    static void decode_id(const uint8_t* idr, CanFrame& frame);

    // CanNode
    bool receive(const CanFrame& frame, const CanNode& from, uint64_t now) override;

    // Periph
    uint64_t next_event() const override;
    void sync(uint64_t now) override;
    void reset() override;
    uint8_t peek(uint16_t offset) override;
    std::span<const DeviceRegister> registers() const override;
    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t offset) override;
    void wr(uint16_t offset, uint8_t value) override;

   private:
    bool accepts(const CanFrame& frame, uint8_t& hit) const;
    void deliver_to_foreground(const CanFrame& frame, uint8_t hit);
    void start_next_transmission(uint64_t now);
    uint64_t frame_ticks(const CanFrame& frame) const;
    void update_irqs(uint64_t now);

    std::array<uint8_t, 0x80> regs_{};
    CanBus* bus_ = nullptr;

    // the transmission in progress: buffer index and when it completes
    int tx_buffer_ = -1;
    uint64_t tx_done_ = kNever;
    // TXE bits cleared by the CPU (buffers queued)
    uint8_t queued_ = 0;

    std::optional<std::pair<CanFrame, uint8_t>> background_;
    uint64_t sent_ = 0;
    uint64_t received_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace sim::sys
