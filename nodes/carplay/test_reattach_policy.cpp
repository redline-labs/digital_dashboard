// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReattachBackoff: when the supervisor runs a phone's bring-up again. The cases
// that matter are a phone that ends CarPlay while plugged in (recovered only by
// running the bring-up again, so it must be prompt) and one that does so
// straight away every time (which must back off, or it loops).
#include "reattach_policy.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

using carplay::AttachOutcome;
using carplay::ReattachBackoff;
using namespace std::chrono_literals;

ReattachBackoff make()
{
    return ReattachBackoff(2000ms, 30000ms, 30000ms);
}

}  // namespace

int main()
{
    {
        ReattachBackoff backoff = make();
        const auto first = backoff.next(AttachOutcome::kFailed, true, 5s);
        expect(first.delay == 4000ms && first.failing, "a failure with the phone attached backs off");
        expect(backoff.next(AttachOutcome::kFailed, true, 5s).delay == 8000ms, "and keeps doubling");
        for (int i = 0; i < 10; ++i)
        {
            backoff.next(AttachOutcome::kFailed, true, 5s);
        }
        expect(backoff.next(AttachOutcome::kFailed, true, 5s).delay == 30000ms, "up to the cap");

        const auto unplugged = backoff.next(AttachOutcome::kFailed, false, 5s);
        expect(unplugged.delay == 2000ms && !unplugged.failing,
               "an unplug resets it, so a replug is picked up at once");
    }

    {
        ReattachBackoff backoff = make();
        backoff.next(AttachOutcome::kFailed, true, 1s);
        const auto restart = backoff.next(AttachOutcome::kRestart, true, 10min);
        expect(restart.delay == 2000ms && !restart.failing,
               "a phone ending a long session is restarted promptly, and not as a failure");
    }

    {
        ReattachBackoff backoff = make();
        const auto quick = backoff.next(AttachOutcome::kRestart, true, 3s);
        expect(quick.delay == 4000ms && quick.failing,
               "a session that drops straight after starting backs off like a failure");
        expect(backoff.next(AttachOutcome::kRestart, true, 3s).delay == 8000ms,
               "so a phone that does it every time does not loop");
    }

    {
        ReattachBackoff backoff = make();
        backoff.next(AttachOutcome::kFailed, true, 1s);
        const auto ended = backoff.next(AttachOutcome::kEnded, true, 1s);
        expect(ended.delay == 2000ms && !ended.failing, "a session that ran and ended resets it");
    }

    if (failures == 0)
    {
        std::printf("all reattach policy checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
