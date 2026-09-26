// SPDX-License-Identifier: GPL-3.0-or-later
//
// SessionStatus: what the dashboard is told as a phone comes and goes. The
// cases worth pinning are the ones a widget or a page trigger would show
// wrongly: a phone mid bring-up reported as absent, a repeated phase
// publishing again, the microphone left on after recording ends, and the
// receiver's late "not recording" undoing an unplug.
#include "session_status.h"

#include <cstdio>
#include <string>
#include <vector>

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

using carplay::SessionPhase;
using carplay::SessionState;
using carplay::SessionStatus;

struct Recorder
{
    std::vector<SessionState> sent;
    SessionStatus status{[this](const SessionState& state) { sent.push_back(state); }};

    const SessionState& last() const { return sent.back(); }
};

}  // namespace

int main()
{
    {
        Recorder r;
        expect(r.status.state().phase == SessionPhase::Idle && !r.status.state().device_connected,
               "starts idle with no phone");
        r.status.republish();
        expect(r.sent.size() == 1 && !r.last().device_connected, "a heartbeat sends the idle state");
    }
    {
        // Every phase between plug-in and unplug counts as connected.
        Recorder r;
        const SessionPhase bring_up[] = {SessionPhase::UsbConfig, SessionPhase::Lockdown, SessionPhase::NcmUp,
                                         SessionPhase::Iap2, SessionPhase::AirplayHandshake, SessionPhase::Error};
        for (const SessionPhase phase : bring_up)
        {
            r.status.setPhase(phase);
            expect(r.last().phase == phase, std::string("publishes ") + carplay::phaseName(phase));
            expect(r.last().device_connected, std::string(carplay::phaseName(phase)) + " is connected");
        }
        expect(r.sent.size() == std::size(bring_up), "one publish per change");

        r.status.setPhase(SessionPhase::Error);
        expect(r.sent.size() == std::size(bring_up), "the same phase again publishes nothing");

        r.status.setPhase(SessionPhase::Idle);
        expect(r.last().phase == SessionPhase::Idle && !r.last().device_connected, "idle is disconnected");
    }
    {
        Recorder r;
        r.status.setPhase(SessionPhase::AirplayHandshake);
        r.status.setRecording(true);
        expect(r.last().phase == SessionPhase::Recording && r.last().device_connected, "recording");
        r.status.setMicrophone(true, 16000, 1);
        expect(r.last().mic_active && r.last().mic_sample_rate_hz == 16000 && r.last().mic_channels == 1,
               "the microphone request is published");

        r.status.setRecording(false);
        expect(r.last().phase == SessionPhase::AirplayHandshake, "losing recording steps back to the handshake");
        expect(!r.last().mic_active && r.last().mic_sample_rate_hz == 0,
               "and drops the microphone request with it");
    }
    {
        // Unplugged: the device watch says Idle, then the receiver's teardown
        // reports recording lost. The phone must stay gone.
        Recorder r;
        r.status.setRecording(true);
        r.status.setMicrophone(true, 16000, 1);
        r.status.setPhase(SessionPhase::Idle);
        expect(!r.last().device_connected && !r.last().mic_active, "an unplug clears connected and the mic");
        const size_t before = r.sent.size();
        r.status.setRecording(false);
        expect(r.status.state().phase == SessionPhase::Idle, "a late 'not recording' does not bring the phone back");
        expect(r.sent.size() == before, "and publishes nothing");
    }
    {
        Recorder r;
        r.status.setDisplay(true, 800, 480);
        r.status.setPhase(SessionPhase::UsbConfig);
        r.status.setPhase(SessionPhase::Idle);
        expect(r.last().night_mode && r.last().main_width_px == 800 && r.last().main_height_px == 480,
               "what the phone was told about the display survives phase changes");
    }

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
