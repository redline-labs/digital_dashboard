// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HELPERS_RATE_GATE_H_
#define HELPERS_RATE_GATE_H_

#include <chrono>

namespace helpers
{

// Minimum spacing between events, as a decision rather than a mechanism: it
// answers "may I send at `now`, and if not how long until I may", and records
// when a send happened. It owns no clock, no timer and no thread -- the caller
// supplies the time, which is what makes every user of it deterministically
// testable.
//
// Note what "never sent" means here. Leaving the last-send timestamp at its
// default would put it at the clock epoch, making a fresh gate look like one
// that sent at time zero and rate limiting the very first event against it.
// Real steady_clock values sit far enough past the epoch to hide that, so it
// only shows up under a reset, a different clock origin, or a test -- which is
// why the state is explicit rather than inferred from the timestamp.
class RateGate
{
  public:
    using clock = std::chrono::steady_clock;

    explicit RateGate(clock::duration min_gap) : _min_gap(min_gap) {}

    // Zero when a send may happen now -- including the first send ever, which
    // has nothing to be spaced against. Otherwise, how long remains.
    clock::duration until(clock::time_point now) const
    {
        if (!_has_sent)
        {
            return clock::duration::zero();
        }
        const auto since = now - _last_sent;
        return (since >= _min_gap) ? clock::duration::zero() : (_min_gap - since);
    }

    bool ready(clock::time_point now) const { return until(now) == clock::duration::zero(); }

    // Records that a send happened at `now`. Separate from ready() on purpose:
    // a caller that decides not to send after asking must not move the gate.
    void mark(clock::time_point now)
    {
        _has_sent = true;
        _last_sent = now;
    }

    // Back to "nothing sent yet", so the next event goes out immediately. For
    // starting a fresh session rather than inheriting the timing of the last.
    void reset() { _has_sent = false; }

    clock::duration minGap() const { return _min_gap; }

  private:
    clock::duration _min_gap;
    bool _has_sent = false;
    clock::time_point _last_sent{};
};

}  // namespace helpers

#endif  // HELPERS_RATE_GATE_H_
