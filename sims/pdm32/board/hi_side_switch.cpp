#include "pdm32/board/hi_side_switch.h"

#include <algorithm>
#include <cmath>

namespace sim::sys {

void HiSideSwitch::solve(bool gate, double battery_v, double load_ohms, double& power) {
    limited_ = false;
    power = 0;
    if (!gate || shutdown_ || battery_v <= 0) {
        v_out_ = 0;
        i_out_ = 0;
        return;
    }
    if (std::isinf(load_ohms)) {
        v_out_ = battery_v;  // open load: the output follows the supply, no current
        i_out_ = 0;
        return;
    }

    const double r_load = std::max(load_ohms, 0.0);
    double i = battery_v / (p_.rds_on + r_load);
    if (i > p_.current_limit) {
        i = p_.current_limit;
        limited_ = true;
    }
    i_out_ = i;
    v_out_ = std::clamp(i * r_load, 0.0, battery_v);
    power = (battery_v - v_out_) * i;
}

void HiSideSwitch::step(double dt, bool gate, double battery_v, double load_ohms) {
    double power = 0;
    solve(gate, battery_v, load_ohms, power);

    // at equilibrium (an idle switch at ambient, an on switch whose junction has settled) there is nothing to
    // integrate: the board stops stepping it until an input changes its operating point
    const double target = p_.ambient + power * p_.r_th;
    if (!shutdown_ && std::fabs(tj_ - target) < 1e-6) {
        tj_ = target;
        steady_ = true;
        return;
    }
    steady_ = false;

    if (dt > 0) {
        // first-order junction temperature toward ambient + P * Rth (the board steps with a fixed dt: alpha cached)
        if (dt != alpha_dt_) {
            alpha_dt_ = dt;
            alpha_ = 1.0 - std::exp(-dt / p_.tau_th);
        }
        tj_ += (target - tj_) * alpha_;

        if (!shutdown_ && tj_ >= p_.t_shutdown) {
            shutdown_ = true;
            shutdowns_++;
        } else if (shutdown_ && tj_ <= p_.t_restart) {
            shutdown_ = false;
        }
        solve(gate, battery_v, load_ohms, power);
    }
}

}  // namespace sim::sys
