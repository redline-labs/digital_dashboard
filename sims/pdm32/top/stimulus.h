// The stimulus: the bridge from the plant (what is wired to the connector) to the board, and the scenario's timed
// events. An Input device, so it is synced before every other device at the same tick: a change it makes at t is
// seen by an ADC conversion that starts at t.
//
// Every step (1 ms) it carries the harness to the board's connector (battery, input sources, load resistances: a lamp's
// changes as its filament heats) and steps the plant with the board's output currents. Events run at their exact
// times. Anything that changes the harness outside a step (an RPC, a test) calls apply() to make it visible at once.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pdm32/board/pdm32.h"
#include "hc08/periph.h"
#include "pdm32/plant/loads.h"

namespace sim::sys {

class Stimulus final : public Periph {
   public:
    Stimulus(Pdm32Board& board, plant::Harness& harness);

    static constexpr uint64_t kStepUs = 1000;

    // Runs `action` at `t_ticks` (before the CPU's instruction at that tick). Events at the same tick run in the order
    // they were added.
    void schedule(uint64_t t_ticks, std::string what, std::function<void()> action);

    // Carries the harness to the board now.
    void apply();

    uint64_t next_event() const override;
    void sync(uint64_t now) override;

    struct Event {
        uint64_t t;
        uint64_t seq;
        std::string what;
        std::function<void()> action;
    };
    const std::vector<Event>& pending() const {
        return events_;
    }

    // The state a snapshot keeps beside the machine's (the events hold functions, so they are copied, not serialised).
    struct State {
        uint64_t next_step;
        uint64_t last_plant;
        uint64_t seq;
        std::vector<Event> events;
    };
    State state() const {
        return State{next_step_, last_plant_, seq_, events_};
    }
    void restore(const State& s) {
        next_step_ = s.next_step;
        last_plant_ = s.last_plant;
        seq_ = s.seq;
        events_ = s.events;
    }

   protected:
    uint8_t rd(uint16_t) override {
        return 0;
    }
    void wr(uint16_t, uint8_t) override {
    }

   private:
    Pdm32Board& board_;
    plant::Harness& harness_;
    uint64_t next_step_ = 0;
    uint64_t last_plant_ = 0;
    uint64_t seq_ = 0;
    std::vector<Event> events_;  // sorted by (t, seq)
};

}  // namespace sim::sys
