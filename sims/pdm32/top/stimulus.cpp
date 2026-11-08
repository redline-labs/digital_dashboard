#include "pdm32/top/stimulus.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace sim::sys {

Stimulus::Stimulus(Pdm32Board& board, plant::Harness& harness)
    : Periph("stimulus", Timing::Input), board_(board), harness_(harness) {
}

void Stimulus::schedule(uint64_t t_ticks, std::string what, std::function<void()> action) {
    Event e{t_ticks, seq_++, std::move(what), std::move(action)};
    const auto at = std::upper_bound(events_.begin(), events_.end(), e, [](const Event& a, const Event& b) {
        return a.t != b.t ? a.t < b.t : a.seq < b.seq;
    });
    events_.insert(at, std::move(e));
}

void Stimulus::apply() {
    board_.set_battery(harness_.battery_v);
    for (size_t k = 0; k < Pdm32Board::kInputs; k++) {
        const plant::InputSource& in = harness_.inputs[k];
        if (in.open) {
            board_.set_input_open(k);
        } else {
            board_.set_input(k, in.volts);
        }
    }
    for (size_t k = 0; k < Pdm32Board::kOutputs; k++) {
        const double r = harness_.loads[k].resistance();
        if (board_.load(k) != r) {
            board_.set_load(k, r);
        }
    }
}

uint64_t Stimulus::next_event() const {
    const uint64_t next_scheduled = events_.empty() ? kNever : events_.front().t;
    return std::min(next_step_, next_scheduled);
}

void Stimulus::sync(uint64_t now) {
    bool changed = false;
    while (!events_.empty() && events_.front().t <= now) {
        Event e = std::move(events_.front());
        events_.erase(events_.begin());
        SPDLOG_DEBUG("stimulus: t={:.4f} {}", double(e.t) / double(m->cfg.tick_hz), e.what);
        e.action();
        changed = true;
    }

    if (now >= next_step_) {
        // the plant over the last step, with the currents the board drove
        const double dt = double(now - last_plant_) / double(m->cfg.tick_hz);
        std::array<double, 32> currents{};
        for (size_t k = 0; k < Pdm32Board::kOutputs; k++) {
            currents[k] = board_.output(k).current();
        }
        harness_.step(dt, currents);
        last_plant_ = now;
        next_step_ = now + kStepUs * m->ticks_per_us();
        changed = true;
    }

    if (changed) {
        apply();
    }
}

}  // namespace sim::sys
