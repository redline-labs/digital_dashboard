// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the CarPlay widget says while there is no picture: every phase has
// something to say, the steps only move forward through a bring-up, and the
// one wait that is on the user says what to do.
#include "carplay/connect_status.h"

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

using Phase = CarPlaySessionState::Phase;
using carplay::connectStatus;

}  // namespace

int main()
{
    expect(connectStatus(false, true, Phase::RECORDING).headline == "CarPlay unavailable",
           "a silent driver is unavailable, whatever it said last");
    expect(connectStatus(true, false, Phase::RECORDING).headline == "Connect an iPhone",
           "no phone asks for one, whatever the phase says");
    expect(connectStatus(true, true, Phase::IDLE).headline == "Connect an iPhone", "idle asks for a phone");

    // Declaration order is bring-up order: the steps must never go backwards
    // through it, and every phase along the way is a step.
    const Phase bring_up[] = {Phase::USB_CONFIG, Phase::LOCKDOWN,          Phase::NCM_UP,
                              Phase::IAP2,       Phase::AIRPLAY_HANDSHAKE, Phase::RECORDING};
    int last_step = 0;
    for (const Phase phase : bring_up)
    {
        const auto status = connectStatus(true, true, phase);
        const std::string name = std::to_string(static_cast<int>(phase));
        expect(!status.headline.empty(), "phase " + name + " has a headline");
        expect(status.step >= 1 && status.step <= carplay::kConnectSteps, "phase " + name + " is a step");
        expect(status.step >= last_step, "phase " + name + " does not step backwards");
        last_step = status.step;
    }
    expect(connectStatus(true, true, Phase::USB_CONFIG).step == 1, "plug-in is the first step");
    expect(last_step == carplay::kConnectSteps, "the last phase before video is the last step");

    expect(connectStatus(true, true, Phase::LOCKDOWN).hint.find("Trust") != std::string_view::npos,
           "pairing tells the user about the Trust prompt");

    const auto error = connectStatus(true, true, Phase::ERROR);
    expect(error.step == 0, "a failure is not a step of progress");
    expect(!error.hint.empty(), "a failure says what happens next");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
