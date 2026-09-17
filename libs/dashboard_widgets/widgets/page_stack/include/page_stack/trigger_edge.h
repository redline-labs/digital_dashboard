// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether one sample of a trigger's expression should change the page.
//
// Runs on the zenoh thread, on EVERY sample. That is the point: the widget
// bindings coalesce to the latest value per 16 ms, and a press and release
// inside one tick would reach them as the release alone -- a lost edge.
#ifndef PAGE_STACK_TRIGGER_EDGE_H_
#define PAGE_STACK_TRIGGER_EDGE_H_

#include "dashboard/page_command.h"

#include <chrono>
#include <cmath>

namespace page_stack
{

class TriggerEdge
{
  public:
    using clock = std::chrono::steady_clock;

    // `stale_after` only matters for `rising`: a gap longer than this makes the
    // next sample a first sample again, so a publisher that restarts with the
    // button held does not fire. Zero -- the default -- never re-primes, which
    // is what a change-only source such as a keypad TPDO needs: after a quiet
    // spell its next message IS the press.
    TriggerEdge(trigger_edge_t edge, std::chrono::milliseconds stale_after)
        : edge_(edge)
        , stale_after_(stale_after)
    {
    }

    // True when this sample fires. A non-finite value is not a sample.
    bool onSample(double value, clock::time_point now)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        const bool truthy = value != 0.0;

        switch (edge_)
        {
            case trigger_edge_t::on_sample:
                primed_ = true;
                return truthy;

            case trigger_edge_t::rising:
            {
                if (primed_ && stale_after_.count() > 0 && now - last_at_ > stale_after_)
                {
                    primed_ = false;
                }
                last_at_ = now;

                if (!primed_)
                {
                    primed_ = true;
                    last_ = truthy;
                    return false;
                }

                const bool fires = truthy && !last_;
                last_ = truthy;
                return fires;
            }
        }
        return false;
    }

    // Has seen a sample. For pages.list: a `rising` trigger that is not primed
    // has never heard from its topic, which is the first thing to rule out when
    // a button "does nothing".
    bool primed() const { return primed_; }

  private:
    trigger_edge_t edge_;
    std::chrono::milliseconds stale_after_;
    bool primed_ = false;
    bool last_ = false;
    clock::time_point last_at_{};
};

}  // namespace page_stack

#endif  // PAGE_STACK_TRIGGER_EDGE_H_
