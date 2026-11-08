// An output's smart high-side switch (a PROFET-type part: the PDM's data sheets are not public, so the parameters are
// typical values, not measured ones): on-resistance, an active current limit, and junction-temperature shutdown with
// automatic restart below a hysteresis. The load is a resistance to ground (infinite: open). The switch's output
// voltage and current are what the PDM's sense multiplexers measure.
//
// A hard short drives the switch into current limit with the whole battery voltage across it; the junction reaches
// shutdown within milliseconds and the switch cycles. Either way the output voltage stays near 0 V, which is what the
// firmware's fault detection looks at (status bit 1 after ~200 ms below 4 V).
#pragma once

#include <cstdint>
#include <limits>

namespace sim::sys {

struct SwitchParams {
    double rds_on = 0.020;        // ohms
    double current_limit = 40.0;  // A
    double r_th = 25.0;           // K/W, junction to board
    double tau_th = 0.010;        // s, junction thermal time constant
    double t_shutdown = 175.0;    // degC
    double t_restart = 150.0;     // degC
    double ambient = 25.0;        // degC

    // the 20 A outputs (1-8) and the 8 A outputs (9-32)
    static SwitchParams high_current() {
        return SwitchParams{0.008, 100.0, 12.0, 0.012};
    }
    static SwitchParams low_current() {
        return SwitchParams{};
    }
};

class HiSideSwitch {
   public:
    static constexpr double kOpen = std::numeric_limits<double>::infinity();

    explicit HiSideSwitch(SwitchParams params = {}) : p_(params), tj_(params.ambient) {
    }

    // Advance by dt seconds with the gate and the supply as given; then solve the operating point.
    void step(double dt, bool gate, double battery_v, double load_ohms);

    double output_voltage() const {
        return v_out_;
    }
    double current() const {
        return i_out_;
    }
    double junction_temperature() const {
        return tj_;
    }
    bool thermal_shutdown() const {
        return shutdown_;
    }
    bool current_limited() const {
        return limited_;
    }
    uint32_t shutdowns() const {
        return shutdowns_;
    }
    // at thermal equilibrium for its present operating point: stepping it changes nothing until an input changes
    bool steady() const {
        return steady_;
    }
    const SwitchParams& params() const {
        return p_;
    }

   private:
    void solve(bool gate, double battery_v, double load_ohms, double& power);

    SwitchParams p_;
    double tj_;
    bool steady_ = true;
    double alpha_dt_ = -1;  // the dt the cached alpha is for
    double alpha_ = 0;      // 1 - exp(-dt / tau)
    double v_out_ = 0;
    double i_out_ = 0;
    bool shutdown_ = false;
    bool limited_ = false;
    uint32_t shutdowns_ = 0;
};

}  // namespace sim::sys
