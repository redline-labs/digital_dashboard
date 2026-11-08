// What is wired to the PDM's connector: the loads on the outputs, the sources on the inputs and the battery. Pure
// models with no knowledge of the board or the CPU; the stimulus (top/) carries their values to the board's
// connector every step.
//
// A load is a resistance that may change with time: a resistor, an open circuit, a short, or a lamp whose filament
// starts cold (about a tenth of its hot resistance) and heats up, which gives the inrush a PDM output must ride
// through.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace sim::plant {

struct Load {
    enum class Kind : uint8_t { Open, Resistive, Short, Lamp };
    Kind kind = Kind::Open;
    double ohms = 0;  // Resistive: the resistance; Lamp: the hot resistance
    double short_ohms = 0.005;
    double lamp_tau = 0.05;  // s, filament heating
    double heat = 0;         // Lamp: 0 cold .. 1 hot

    static Load open() {
        return Load{};
    }
    static Load resistive(double ohms) {
        return Load{Kind::Resistive, ohms};
    }
    static Load hard_short() {
        return Load{Kind::Short};
    }
    static Load lamp(double hot_ohms) {
        return Load{Kind::Lamp, hot_ohms};
    }

    // the resistance now (infinity: open)
    double resistance() const;
    // advance the filament by dt with `current` flowing
    void step(double dt, double current);
    std::string describe() const;
};

struct InputSource {
    bool open = true;  // nothing connected: the PDM's pull-up decides
    double volts = 0;
};

// Parses "open", "short", "6.0" / "6R" (ohms), "lamp:2.4" (hot ohms).
Load parse_load(const std::string& text);

struct Harness {
    double battery_v = 13.5;
    std::array<Load, 32> loads{};
    std::array<InputSource, 23> inputs{};

    void step(double dt, const std::array<double, 32>& output_currents);
};

}  // namespace sim::plant
