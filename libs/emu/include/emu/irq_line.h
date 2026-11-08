// A device's interrupt source on the interrupt controller.
//
// The controller (hc08/irq.h InterruptController, implemented by the SoC) is an IrqSink; a device holds one
// IrqLine per source it raises and is handed it when the board is wired. A default-constructed line is unconnected and
// set() does nothing, as a device on a machine without a controller always behaved. IrqSink lives here, not in cpu/,
// because wire/ may not include cpu/ and the line must be usable from every layer.
#pragma once

#include <cstdint>
#include <functional>

#include "emu/common.h"

namespace sim::sys {

class IrqSink {
   public:
    // source `src` raised or lowered
    virtual void set_line(int src, bool on) = 0;

   protected:
    ~IrqSink() = default;
};

// The fault point on a line: the level the sink sees for the level the device drove, at sim time `now`. Empty unless a
// fault is attached; an empty tap costs one test.
using IrqTap = std::function<bool(bool level, uint64_t now)>;

class IrqLine {
   public:
    IrqLine() = default;
    IrqLine(IrqSink* sink, int src) : sink_(sink), src_(src) {
    }

    bool connected() const {
        return sink_ != nullptr;
    }

    int source() const {
        return src_;
    }

    void set(bool on, uint64_t now) const {
        if (sink_ != nullptr) {
            sink_->set_line(src_, tap ? tap(on, now) : on);
        }
    }

    IrqTap tap;

   private:
    IrqSink* sink_ = nullptr;
    int src_ = -1;
};

}  // namespace sim::sys
