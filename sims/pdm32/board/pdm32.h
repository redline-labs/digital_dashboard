// The PDM32 board: the GZ60 and everything between it and the connector.
//
//   outputs  32 high-side switches (1-8: 20 A, 9-32: 8 A), gated by port pins (board/wiring.h), each with a load
//            resistance set from the connector side
//   inputs   23 inputs, 0-51 V, each with a 10 k pull-up to the battery (an open input reads the battery)
//   sensing  the multiplexed ADC: the board answers the ADC's sample with the scaled analog of the selected signal
//   supply   battery voltage, the internal 9.5 V rail, board temperature
//   CAN      the MSCAN on the board's CAN bus (the connector's CAN pair)
//
// The board is a Timed device: while any switch is away from thermal equilibrium it advances the junction
// temperatures every 100 us, so a shorted output limits, overheats and cycles in time; when all are settled it
// schedules nothing (the next input change wakes it). The connector side (set_battery, set_input, set_load) is driven by the
// stimulus (top/) from the plant (plant/loads).
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "pdm32/board/hi_side_switch.h"
#include "pdm32/board/wiring.h"
#include "gz60/soc.h"
#include "emu/can_bus.h"

namespace sim::sys {

class Pdm32Board final : public Periph {
   public:
    static constexpr size_t kOutputs = 32;
    static constexpr size_t kInputs = 23;
    static constexpr double kOpen = HiSideSwitch::kOpen;

    explicit Pdm32Board(MachineConfig config = {});

    Gz60 soc;
    CanBus can;

    // ------------------------------------------------------------------ the connector side
    void set_battery(double volts);
    // a driven input (volts), or open (NaN: the pull-up to the battery)
    void set_input(size_t index, double volts);
    void set_input_open(size_t index);
    void set_load(size_t index, double ohms);
    void set_board_temperature(double celsius) {
        board_temp_c_ = celsius;
    }

    double battery() const {
        return battery_v_;
    }
    double input_voltage(size_t index) const;
    bool input_open(size_t index) const;
    double load(size_t index) const {
        return load_ohms_[index];
    }

    // ------------------------------------------------------------------ what the connector sees
    bool gate(size_t index) const {
        return gates_[index];
    }
    const HiSideSwitch& output(size_t index) const {
        return switches_[index];
    }

    // The current-sense hardware of output `index`: the raw ADC count it produces for each calibrated count (the
    // inverse of the unit's current calibration, so the firmware reports the true current). Identity by default.
    void set_current_sense(size_t index, const std::array<uint8_t, 256>& raw_for_count) {
        raw_for_count_[index] = raw_for_count;
    }

    // The ADC pin voltage for (channel, mux address): the scaled analog of the signal the multiplexers select.
    double adc_pin(int channel, uint8_t mux) const;

    // Periph: the 100 us thermal step
    static constexpr uint64_t kStepUs = 100;
    uint64_t next_event() const override;
    void sync(uint64_t now) override;
    void save(std::vector<uint8_t>& out) const override;
    void load(const uint8_t*& in) override;

   protected:
    uint8_t rd(uint16_t) override {
        return 0;
    }
    void wr(uint16_t, uint8_t) override {
    }

   private:
    void update_gates(uint64_t now);
    void advance(uint64_t now);
    void solve_now(uint64_t now);

    double battery_v_ = 13.5;
    double board_temp_c_ = 25.0;
    std::array<double, kInputs> input_v_{};
    std::array<double, kOutputs> load_ohms_{};
    std::array<bool, kOutputs> gates_{};
    std::array<HiSideSwitch, kOutputs> switches_;
    std::array<std::array<uint8_t, 256>, kOutputs> raw_for_count_;  // configuration, not state: not in snapshots
    uint64_t last_step_ = 0;
    bool stepping_ = false;                          // some switch is away from equilibrium
    std::array<uint8_t, kPortCount> last_driven_{};  // what each port drove when the gates were last updated
};

}  // namespace sim::sys
