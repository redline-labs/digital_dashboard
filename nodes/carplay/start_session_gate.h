// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_START_SESSION_GATE_H_
#define CARPLAY_START_SESSION_GATE_H_

#include <chrono>

namespace carplay
{

// Holds CarPlayStartSession until there is an address to put in it.
//
// The address is the NCM interface's IPv6 link-local, and the kernel only
// generates one once the PHONE raises carrier on that link. Requiring it before
// iAP2 started meant a phone that raises carrier late -- or only after it has
// identified us -- never got an iAP2 session at all: we waited for the address
// before doing the thing that produces it. So the request is remembered and
// retried from the poll loop, and only gives up after `patience`.
class StartSessionGate
{
  public:
    using clock = std::chrono::steady_clock;

    enum class Action
    {
        kNone,
        kSend,
        kGiveUp,
    };

    explicit StartSessionGate(std::chrono::milliseconds patience) : patience_(patience) {}

    // The phone reported wired CarPlay available. A repeat while still waiting
    // keeps the first deadline, so a phone that re-announces cannot hold the
    // session open forever; a repeat after a send re-arms, as each
    // announcement was answered before this class existed.
    void request(clock::time_point now)
    {
        if (!pending_)
        {
            pending_ = true;
            since_ = now;
        }
    }

    // Ready wins over the deadline: an address that turns up on the very poll
    // that would have given up is still an address.
    Action poll(clock::time_point now, bool endpoint_ready)
    {
        if (!pending_)
        {
            return Action::kNone;
        }
        if (endpoint_ready)
        {
            pending_ = false;
            return Action::kSend;
        }
        if (now - since_ >= patience_)
        {
            pending_ = false;
            return Action::kGiveUp;
        }
        return Action::kNone;
    }

    bool pending() const { return pending_; }

  private:
    std::chrono::milliseconds patience_;
    bool pending_ = false;
    clock::time_point since_{};
};

}  // namespace carplay

#endif  // CARPLAY_START_SESSION_GATE_H_
