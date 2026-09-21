// SPDX-License-Identifier: GPL-3.0-or-later
//
// StartSessionGate: CarPlayStartSession waits for the NCM link-local instead of
// the whole bring-up failing without one. The cases worth pinning are the ones
// that fail silently on hardware: sending with no address, never sending once
// one appears, and a re-announcing phone pushing the deadline out forever.
#include "start_session_gate.h"

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

using carplay::StartSessionGate;
using Action = StartSessionGate::Action;
using namespace std::chrono_literals;

const StartSessionGate::clock::time_point t0{};

}  // namespace

int main()
{
    {
        StartSessionGate gate(30s);
        expect(gate.poll(t0, true) == Action::kNone, "nothing is sent before the phone asks");
        expect(!gate.pending(), "idle gate is not pending");
    }
    {
        StartSessionGate gate(30s);
        gate.request(t0);
        expect(gate.poll(t0, true) == Action::kSend, "address already there: send at once");
        expect(gate.poll(t0 + 1s, true) == Action::kNone, "and only once");
    }
    {
        // The case this exists for: carrier comes up after the phone asked.
        StartSessionGate gate(30s);
        gate.request(t0);
        expect(gate.poll(t0, false) == Action::kNone, "no address: do not send");
        expect(gate.pending(), "still waiting");
        expect(gate.poll(t0 + 10s, false) == Action::kNone, "still no address: keep waiting");
        expect(gate.poll(t0 + 12s, true) == Action::kSend, "address appeared: send");
        expect(!gate.pending(), "sent, so no longer pending");
    }
    {
        StartSessionGate gate(30s);
        gate.request(t0);
        expect(gate.poll(t0 + 29s, false) == Action::kNone, "inside patience");
        expect(gate.poll(t0 + 30s, false) == Action::kGiveUp, "patience exhausted: give up");
        expect(gate.poll(t0 + 31s, true) == Action::kNone, "gave up: a late address sends nothing");
    }
    {
        StartSessionGate gate(30s);
        gate.request(t0);
        expect(gate.poll(t0 + 30s, true) == Action::kSend, "ready beats the deadline on the same poll");
    }
    {
        // A phone that re-announces availability must not extend the wait.
        StartSessionGate gate(30s);
        gate.request(t0);
        gate.request(t0 + 20s);
        expect(gate.poll(t0 + 30s, false) == Action::kGiveUp, "a repeat keeps the first deadline");
    }
    {
        StartSessionGate gate(30s);
        gate.request(t0);
        expect(gate.poll(t0, true) == Action::kSend, "first announcement answered");
        gate.request(t0 + 5s);
        expect(gate.poll(t0 + 5s, true) == Action::kSend, "a later announcement is answered again");
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
