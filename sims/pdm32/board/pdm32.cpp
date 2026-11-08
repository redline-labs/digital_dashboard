#include "pdm32/board/pdm32.h"

#include <algorithm>
#include <cmath>

#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {

// a scaled value (in ADC counts of the firmware's 8-bit reading) as the ADC pin voltage that reads as it
double pin_for_count(double count) {
    const double clamped = std::clamp(count, 0.0, 255.75);
    return (clamped * 4.0 + 0.5) / 1023.0 * 5.0;
}

constexpr double kInputVoltsPerCount = 0.2;
constexpr double kOutputVoltsPerCount = 0.2;
constexpr double kBatteryVoltsPerCount = 0.1216;
constexpr double kRailVoltsPerCount = 0.0615;
constexpr double kRailVolts = 9.5;

// The firmware's board-temperature table is linear: 0 degC at index 87, ~0.82 degC per index, and the index it uses
// settles at 4 x the raw count (a unit at room temperature reads raw 27, about 19 degC).
double temperature_count(double celsius) {
    return (87.3 + celsius / 0.8203) / 4.0;
}

std::array<HiSideSwitch, 32> make_switches() {
    std::array<HiSideSwitch, 32> s;
    for (size_t k = 0; k < 32; k++) {
        s[k] = HiSideSwitch(k < 8 ? SwitchParams::high_current() : SwitchParams::low_current());
    }
    return s;
}

// count -> raw through the sense table, linear between whole counts (the identity table passes fractions through)
double sensed_count(const std::array<uint8_t, 256>& raw_for_count, double count) {
    const double c = std::clamp(count, 0.0, 255.0);
    // c is clamped to [0, 255], so its integral part indexes the table.
    const size_t n = std::min<size_t>(static_cast<size_t>(c), 254);
    const double frac = c - static_cast<double>(n);
    return raw_for_count[n] + frac * (double(raw_for_count[n + 1]) - double(raw_for_count[n]));
}

}  // namespace

Pdm32Board::Pdm32Board(MachineConfig config) : Periph("board", Timing::Timed), soc(config), switches_(make_switches()) {
    input_v_.fill(std::numeric_limits<double>::quiet_NaN());  // open
    for (std::array<uint8_t, 256>& table : raw_for_count_) {
        for (size_t c = 0; c < 256; c++) {
            table[c] = uint8_t(c);
        }
    }
    load_ohms_.fill(kOpen);

    soc.m.add(std::shared_ptr<Periph>(std::shared_ptr<Periph>{}, this));
    soc.mscan->attach(can);
    soc.ports->on_drive = [this](Port port, uint8_t level, uint8_t ddr, uint64_t now) {
        // only the ports with gates, and only when what they drive changed (PTB carries the mux address and changes
        // on every ADC conversion)
        const size_t p = size_t(port);
        const uint8_t driven = uint8_t(level & ddr);
        if (port == Port::B || port == Port::G || driven == last_driven_[p]) {
            return;
        }
        last_driven_[p] = driven;
        update_gates(now);
    };
    soc.adc->sample = [this](int channel, uint64_t now) {
        sync(now);
        return adc_pin(channel, uint8_t(soc.ports->driven(Port::B) & 0x13));
    };
}

void Pdm32Board::set_battery(double volts) {
    volts = std::max(0.0, volts);
    if (volts == battery_v_) {
        return;
    }
    battery_v_ = volts;
    solve_now(soc.m.now());
}

void Pdm32Board::set_input(size_t index, double volts) {
    input_v_.at(index) = volts;
}

void Pdm32Board::set_input_open(size_t index) {
    input_v_.at(index) = std::numeric_limits<double>::quiet_NaN();
}

bool Pdm32Board::input_open(size_t index) const {
    return std::isnan(input_v_.at(index));
}

double Pdm32Board::input_voltage(size_t index) const {
    // open: the 10 k pull-up to the battery
    return input_open(index) ? battery_v_ : std::clamp(input_v_.at(index), 0.0, 51.0);
}

void Pdm32Board::set_load(size_t index, double ohms) {
    load_ohms_.at(index) = ohms;
    solve_now(soc.m.now());
}

void Pdm32Board::update_gates(uint64_t now) {
    advance(now);
    for (size_t k = 0; k < kOutputs; k++) {
        const pdm32::Pin pin = pdm32::kOutputPins[k];
        gates_[k] = ((soc.ports->driven(pin.port) >> pin.bit) & 1) != 0;
    }
    solve_now(now);
}

void Pdm32Board::advance(uint64_t now) {
    if (now <= last_step_) {
        return;
    }
    if (stepping_) {
        const double dt = double(now - last_step_) / double(soc.m.cfg.tick_hz);
        bool any = false;
        for (size_t k = 0; k < kOutputs; k++) {
            if (!switches_[k].steady()) {
                switches_[k].step(dt, gates_[k], battery_v_, load_ohms_[k]);
                any = any || !switches_[k].steady();
            }
        }
        stepping_ = any;
    }
    last_step_ = now;
}

void Pdm32Board::solve_now(uint64_t now) {
    advance(now);
    bool any = false;
    for (size_t k = 0; k < kOutputs; k++) {
        switches_[k].step(0, gates_[k], battery_v_, load_ohms_[k]);
        any = any || !switches_[k].steady();
    }
    stepping_ = any;
    // a switch that left equilibrium needs thermal steps from now: tell the machine (the change came through another
    // device's access, or from outside a run)
    soc.m.reschedule(next_event());
}

uint64_t Pdm32Board::next_event() const {
    // a thermal step only while some switch is away from equilibrium
    return stepping_ ? last_step_ + kStepUs * soc.m.ticks_per_us() : kNever;
}

void Pdm32Board::sync(uint64_t now) {
    advance(now);
}

double Pdm32Board::adc_pin(int channel, uint8_t mux) const {
    const pdm32::SenseSource s = pdm32::sense_source(channel, mux);
    switch (s.kind) {
        case pdm32::Sense::Battery:
            return pin_for_count(battery_v_ / kBatteryVoltsPerCount);
        case pdm32::Sense::Rail9V5:
            return pin_for_count(battery_v_ > 6.0 ? kRailVolts / kRailVoltsPerCount : 0.0);
        case pdm32::Sense::BoardTemp:
            return pin_for_count(temperature_count(board_temp_c_));
        case pdm32::Sense::InputVoltage:
            return pin_for_count(input_voltage(s.index) / kInputVoltsPerCount);
        case pdm32::Sense::OutputVoltage:
            return pin_for_count(switches_[s.index].output_voltage() / kOutputVoltsPerCount);
        case pdm32::Sense::OutputCurrent: {
            const double amps_per_count = s.index < 8 ? 0.5 : 0.2;
            return pin_for_count(sensed_count(raw_for_count_[s.index], switches_[s.index].current() / amps_per_count));
        }
        case pdm32::Sense::None:
            return 0.0;
    }
    return 0.0;
}

void Pdm32Board::save(std::vector<uint8_t>& out) const {
    put_value(out, battery_v_);
    put_value(out, board_temp_c_);
    put_value(out, input_v_);
    put_value(out, load_ohms_);
    put_value(out, gates_);
    put_value(out, switches_);
    put_value(out, last_step_);
    put_value(out, stepping_);
    put_value(out, last_driven_);
}

void Pdm32Board::load(const uint8_t*& in) {
    get_value(in, battery_v_);
    get_value(in, board_temp_c_);
    get_value(in, input_v_);
    get_value(in, load_ohms_);
    get_value(in, gates_);
    get_value(in, switches_);
    get_value(in, last_step_);
    get_value(in, stepping_);
    get_value(in, last_driven_);
}

}  // namespace sim::sys
