// SPDX-License-Identifier: GPL-3.0-or-later
//
// ScreenHandover: what the node tells the phone and the dashboard as CarPlay is
// shown and hidden and the phone takes the screen back. The cases worth pinning
// are the ones that would fight: a heartbeat re-sending take, a take after the
// phone reclaimed, a refused take bouncing the dashboard back to CarPlay.
#include "screen_handover.h"

#include <spdlog/spdlog.h>

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

using carplay::ScreenHandover;
using Command = ScreenHandover::Command;
using Event = ScreenHandover::Event;
using Entity = airplay::ScreenEntity;
using namespace std::chrono_literals;

const ScreenHandover::clock::time_point t0{};

ScreenHandover enabled()
{
    ScreenHandover::Config config;
    config.enabled = true;
    config.visibility_stale_after = 3000ms;
    return ScreenHandover(config);
}

bool only(const ScreenHandover::Actions& actions, Command command)
{
    return actions.events.empty() && actions.commands.size() == 1 && actions.commands[0] == command;
}

bool hasCommand(const ScreenHandover::Actions& actions, Command command)
{
    for (Command c : actions.commands)
    {
        if (c == command) return true;
    }
    return false;
}

bool onlyEvent(const ScreenHandover::Actions& actions, Event event)
{
    return actions.commands.empty() && actions.events.size() == 1 && actions.events[0] == event;
}

void testHideTakesAndShowGivesBack()
{
    auto h = enabled();
    expect(h.onRecording(true).empty(), "a session starting with CarPlay visible asks for nothing");
    expect(h.onVisibility(true, t0).empty(), "a visible heartbeat asks for nothing");
    expect(only(h.onVisibility(false, t0 + 1s), Command::take_screen), "hiding CarPlay takes the screen");
    expect(h.onVisibility(false, t0 + 2s).empty(), "a hidden heartbeat does not take again");
    expect(h.onScreenOwner(Entity::accessory).empty(), "the phone's acknowledgement is not an event");

    const auto shown = h.onVisibility(true, t0 + 3s);
    expect(hasCommand(shown, Command::untake_screen) && hasCommand(shown, Command::request_keyframe),
           "showing CarPlay gives the screen back and asks for a keyframe");
    expect(!hasCommand(shown, Command::request_ui), "without requestUI unless configured");
    expect(!h.carHoldsScreen(), "and the car no longer holds it");
}

void testHideBeforeRecordingWaitsForTheSession()
{
    auto h = enabled();
    expect(h.onVisibility(false, t0).empty(), "nothing can be sent before the session records");
    expect(only(h.onRecording(true), Command::take_screen), "the take goes out when recording starts");
}

void testSiriReclaimsAndTheDashboardIsTold()
{
    auto h = enabled();
    h.onRecording(true);
    h.onVisibility(false, t0);
    h.onScreenOwner(Entity::accessory);

    expect(onlyEvent(h.onScreenOwner(Entity::controller), Event::screen_requested),
           "the phone taking the screen back while hidden asks the dashboard for CarPlay");
    expect(h.onVisibility(false, t0 + 1s).empty(), "a hidden heartbeat after a reclaim does not fight for it");
    expect(h.onVisibility(true, t0 + 2s).empty(), "showing CarPlay then has nothing to give back");
    expect(only(h.onVisibility(false, t0 + 3s), Command::take_screen), "and a later hide takes it again");
}

void testABorrowReturnedWhileHidden()
{
    auto h = enabled();
    h.onRecording(true);
    h.onVisibility(false, t0);
    h.onScreenOwner(Entity::accessory);
    h.onScreenOwner(Entity::controller);  // Siri
    // The dashboard did not switch (no trigger configured), and Siri ended.
    expect(onlyEvent(h.onScreenOwner(Entity::accessory), Event::screen_released),
           "the screen coming back while still hidden is reported");
    expect(h.carHoldsScreen(), "and the car holds it again");
    expect(hasCommand(h.onVisibility(true, t0 + 1s), Command::untake_screen),
           "so showing CarPlay afterwards gives it back like any other take");
}

void testARefusedTakeDoesNotBounceTheDashboard()
{
    auto h = enabled();
    h.onRecording(true);
    h.onVisibility(false, t0);
    expect(h.onScreenOwner(Entity::controller).empty(), "a refused take is not screen_requested");
    expect(h.onVisibility(false, t0 + 1s).empty(), "and is not retried on every heartbeat");
}

void testStaleVisibilityGivesTheScreenBack()
{
    auto h = enabled();
    h.onRecording(true);
    h.onVisibility(false, t0);
    h.onScreenOwner(Entity::accessory);
    expect(h.tick(t0 + 2s).empty(), "within the timeout nothing happens");
    expect(hasCommand(h.tick(t0 + 4s), Command::untake_screen), "a silent dashboard gets the screen given back");
    expect(h.dashboardShowsCarPlay(), "because silence counts as visible");
    expect(h.tick(t0 + 5s).empty(), "and only once");

    ScreenHandover never_heard = enabled();
    never_heard.onRecording(true);
    expect(never_heard.tick(t0 + 60s).empty(), "a dashboard never heard from is visible from the start");
}

void testDisabledAsksForNothing()
{
    ScreenHandover h(ScreenHandover::Config{});
    expect(h.onRecording(true).empty(), "disabled: recording");
    expect(h.onVisibility(false, t0).empty(), "disabled: hidden");
    expect(h.onScreenOwner(Entity::controller).empty(), "disabled: owner change");
    expect(h.onVisibility(true, t0 + 1s).empty(), "disabled: shown");
    expect(!h.carHoldsScreen(), "disabled never holds the screen");
}

void testSessionEndResets()
{
    auto h = enabled();
    h.onRecording(true);
    h.onVisibility(false, t0);
    expect(h.onRecording(false).empty(), "a session ending sends nothing -- there is no one to send to");
    expect(!h.carHoldsScreen(), "and forgets the take");
    expect(only(h.onRecording(true), Command::take_screen), "a new session with CarPlay still hidden takes again");
}

void testRequestUiWhenConfigured()
{
    ScreenHandover::Config config;
    config.enabled = true;
    config.request_ui_on_show = true;
    ScreenHandover h(config);
    h.onRecording(true);
    h.onVisibility(false, t0);
    expect(hasCommand(h.onVisibility(true, t0 + 1s), Command::request_ui), "request_ui_on_show adds requestUI");
}

void testFlapping()
{
    auto h = enabled();
    h.onRecording(true);
    int takes = 0;
    int untakes = 0;
    for (int i = 0; i < 10; ++i)
    {
        for (Command c : h.onVisibility(i % 2 == 0 ? false : true, t0 + std::chrono::milliseconds(i * 10)).commands)
        {
            takes += c == Command::take_screen ? 1 : 0;
            untakes += c == Command::untake_screen ? 1 : 0;
        }
    }
    expect(takes == 5 && untakes == 5, "every hide takes once and every show gives back once");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);
    testHideTakesAndShowGivesBack();
    testHideBeforeRecordingWaitsForTheSession();
    testSiriReclaimsAndTheDashboardIsTold();
    testABorrowReturnedWhileHidden();
    testARefusedTakeDoesNotBounceTheDashboard();
    testStaleVisibilityGivesTheScreenBack();
    testDisabledAsksForNothing();
    testSessionEndResets();
    testRequestUiWhenConfigured();
    testFlapping();
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
