// A signal between two chips: a digital level (Net<bool>) or an analog quantity (Net<double>, volts).
//
// One driver sets it; any number of listeners are told when the value changes. The listeners run in the order they
// were added, which is how a board keeps the call order of the chips that see one event (the supply falling reaches the
// EEPROM before the MCU). A device holds the nets it was given and never reaches for the device on the other side.
// Time is passed in by the driver (the machine's time in ticks) so wire/ needs nothing from cpu/.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "emu/common.h"

namespace sim::sys {

template <class T>
class Net {
   public:
    using Listener = std::function<void(T level, uint64_t now)>;
    // The fault point: the value the listeners see for the value the driver set. Empty unless a fault is attached; an
    // empty tap costs one test.
    using Tap = std::function<T(T level, uint64_t now)>;

    explicit Net(T initial = T{}) : level_(initial) {
    }

    // listeners capture addresses of the devices on the net
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    T level() const {
        return level_;
    }

    // The driver sets the value (through the tap, if any); a change is stored first, then told to every listener.
    void set(T value, uint64_t now) {
        if (tap) {
            value = tap(value, now);
        }

        if (value == level_) {
            return;
        }

        level_ = value;
        notify(now);
    }

    void listen(Listener fn) {
        listeners_.push_back(std::move(fn));
    }

    // Tell the listeners the current value without a change: the wiring pushes the initial state onto the chips.
    void publish(uint64_t now) const {
        notify(now);
    }

    // Snapshot load: set the value without telling anyone or applying the tap; the devices on the net restore their
    // own state.
    void restore(T value) {
        level_ = value;
    }

    Tap tap;

   private:
    void notify(uint64_t now) const {
        for (const Listener& fn : listeners_) {
            fn(level_, now);
        }
    }

    T level_;
    std::vector<Listener> listeners_;
};

}  // namespace sim::sys
