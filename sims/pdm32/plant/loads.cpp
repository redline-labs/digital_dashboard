#include "pdm32/plant/loads.h"

#include <cmath>
#include <stdexcept>

namespace sim::plant {

double Load::resistance() const {
    switch (kind) {
        case Kind::Open:
            return std::numeric_limits<double>::infinity();
        case Kind::Resistive:
            return ohms;
        case Kind::Short:
            return short_ohms;
        case Kind::Lamp:
            return ohms * (0.1 + 0.9 * heat);
    }
    return std::numeric_limits<double>::infinity();
}

void Load::step(double dt, double current) {
    if (kind != Kind::Lamp) {
        return;
    }
    const double target = current > 0.01 ? 1.0 : 0.0;
    heat += (target - heat) * (1.0 - std::exp(-dt / lamp_tau));
}

std::string Load::describe() const {
    switch (kind) {
        case Kind::Open:
            return "open";
        case Kind::Resistive:
            return std::to_string(ohms) + " ohm";
        case Kind::Short:
            return "short";
        case Kind::Lamp:
            return "lamp " + std::to_string(ohms) + " ohm hot";
    }
    return "?";
}

Load parse_load(const std::string& text) {
    if (text == "open") {
        return Load::open();
    }
    if (text == "short") {
        return Load::hard_short();
    }
    if (text.rfind("lamp:", 0) == 0) {
        return Load::lamp(std::stod(text.substr(5)));
    }
    std::string number = text;
    if (!number.empty() && (number.back() == 'R' || number.back() == 'r')) {
        number.pop_back();
    }
    try {
        size_t used = 0;
        const double ohms = std::stod(number, &used);
        if (used == number.size() && ohms >= 0) {
            return Load::resistive(ohms);
        }
    } catch (const std::exception&) {
    }
    throw std::runtime_error("load '" + text + "': expected open, short, lamp:<ohms> or <ohms>");
}

void Harness::step(double dt, const std::array<double, 32>& output_currents) {
    for (size_t k = 0; k < loads.size(); k++) {
        loads[k].step(dt, output_currents[k]);
    }
}

}  // namespace sim::plant
