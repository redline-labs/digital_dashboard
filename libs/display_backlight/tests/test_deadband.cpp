// When the backlight node publishes before its heartbeat: every boundary of
// the lux and temperature bands, and the changes that always count.

#include "display_backlight/deadband.h"

#include <cstdio>
#include <string>

namespace
{

using namespace display_backlight;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void testLux()
{
    check(!luxMoved(100.0, 104.9), "4.9 % brighter is inside the band");
    check(luxMoved(100.0, 105.0), "5 % brighter is out");
    check(luxMoved(100.0, 95.0), "5 % darker is out");
    check(!luxMoved(100.0, 95.1), "4.9 % darker is in");
    check(!luxMoved(0.0, 0.04), "at dark the band is 5 % of 1 lux, not of zero");
    check(luxMoved(0.0, 0.05), "and a step past it counts");
    check(!luxMoved(0.5, 0.54), "the floor holds below 1 lux");
    check(luxMoved(std::nullopt, 3.0), "a sensor coming back counts");
    check(luxMoved(3.0, std::nullopt), "a sensor going missing counts");
    check(!luxMoved(std::nullopt, std::nullopt), "missing and still missing does not");
}

void testTemperature()
{
    check(!temperatureMoved(45.0, 45.49), "under half a degree is in");
    check(temperatureMoved(45.0, 45.5), "half a degree is out");
    check(temperatureMoved(-5.0, -5.5), "below zero, downwards");
    check(temperatureMoved(std::nullopt, 20.0) && temperatureMoved(20.0, std::nullopt), "presence changes count");
    check(!temperatureMoved(std::nullopt, std::nullopt), "missing and still missing does not");
}

StatusValues board()
{
    StatusValues values;
    values.backlight = BacklightStatus{};
    values.backlight->brightness = 24576;
    values.backlight->actualBrightness = 24576;
    values.lux = {3.42, 2.12};
    values.celsius = {45.187, 44.5};
    return values;
}

void testWholeStatus()
{
    check(!movedPastDeadband(board(), board()), "the same status does not publish");

    StatusValues noise = board();
    noise.lux[0] = 3.44;
    noise.celsius[1] = 44.7;
    check(!movedPastDeadband(board(), noise), "noise inside every band does not publish");

    StatusValues brighter = board();
    brighter.backlight->actualBrightness = 24577;
    check(movedPastDeadband(board(), brighter), "any backlight change publishes, however small");

    StatusValues fault = board();
    fault.backlight->faults = std::vector<uint16_t>{0, 0, 0x0800};
    check(movedPastDeadband(board(), fault), "a fault word appearing publishes");

    StatusValues unread = board();
    unread.backlight.reset();
    check(movedPastDeadband(board(), unread), "the backlight becoming unreadable publishes");

    StatusValues lux = board();
    lux.lux[1] = 3.0;
    check(movedPastDeadband(board(), lux), "one lux sensor past its band publishes");

    StatusValues warm = board();
    warm.celsius[0] = 46.0;
    check(movedPastDeadband(board(), warm), "one temperature past its band publishes");

    StatusValues fewer = board();
    fewer.celsius.pop_back();
    check(movedPastDeadband(board(), fewer), "a different number of channels publishes");
}

}  // namespace

int main()
{
    testLux();
    testTemperature();
    testWholeStatus();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
