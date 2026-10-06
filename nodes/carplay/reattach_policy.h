// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_REATTACH_POLICY_H_
#define CARPLAY_REATTACH_POLICY_H_

#include <algorithm>
#include <chrono>

namespace carplay
{

// How one attachment of a phone ended, as the supervisor sees it.
enum class AttachOutcome
{
    kFailed,   // the bring-up did not complete
    kEnded,    // it ran and ended: the phone went, or the node is stopping
    kRestart,  // it ran, and the phone ended CarPlay with USB still attached
};

// How long the supervisor waits before running a phone's bring-up again.
//
// A failure backs off, so a phone whose owner tapped "Don't Trust" is not
// re-run every two seconds forever. A restart -- the phone dropping CarPlay
// while still plugged in, which only a fresh iAP2 session recovers -- is retried
// promptly, but only if the session it ends had run for a while: one that drops
// straight after starting is a failure that happens to reach RECORD, and
// retrying it promptly would loop.
class ReattachBackoff
{
  public:
    using duration = std::chrono::milliseconds;

    struct Decision
    {
        duration delay;
        // Whether this counts against the phone: the dashboard is told the
        // bring-up is failing rather than merely restarting.
        bool failing = false;
    };

    ReattachBackoff(duration initial, duration max, duration stable) :
        initial_(initial), max_(max), stable_(stable), current_(initial)
    {
    }

    Decision next(AttachOutcome outcome, bool phone_present, std::chrono::steady_clock::duration ran_for)
    {
        const bool stable_restart = outcome == AttachOutcome::kRestart && ran_for >= stable_;
        if (!phone_present || outcome == AttachOutcome::kEnded || stable_restart)
        {
            current_ = initial_;
            return {initial_, false};
        }
        current_ = std::min(current_ * 2, max_);
        return {current_, true};
    }

  private:
    duration initial_;
    duration max_;
    duration stable_;
    duration current_;
};

}  // namespace carplay

#endif  // CARPLAY_REATTACH_POLICY_H_
