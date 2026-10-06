// SPDX-License-Identifier: GPL-3.0-or-later
//
// VehicleStatusFeed: config values as the start, live values over them key by
// key, and a change sent once while the phone is subscribed.
#include "vehicle_status_feed.h"

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

using carplay::VehicleStatus;
using carplay::VehicleStatusFeed;

}  // namespace

int main()
{
    {
        VehicleStatusFeed none({}, false);
        expect(!none.advertised(), "nothing configured and no live source: not advertised");
        VehicleStatusFeed live({}, true);
        expect(live.advertised(), "a live source is advertised before it has said anything");
        live.subscribed();
        expect(!live.takeChange(), "and sends nothing until it has");
    }

    VehicleStatus baseline;
    baseline.range_km = 420;
    baseline.outside_temperature_c = 21;
    VehicleStatusFeed feed(baseline, true);
    feed.subscribed();
    const auto first = feed.takeChange();
    expect(first && first->range_km == 420 && first->outside_temperature_c == 21,
           "the configured values go out on subscription");
    expect(!feed.takeChange(), "once, not on every poll");

    VehicleStatus range_only;
    range_only.range_km = 380;
    feed.apply(range_only);
    const auto moved = feed.takeChange();
    expect(moved && moved->range_km == 380, "a live range replaces the configured one");
    expect(moved && moved->outside_temperature_c == 21,
           "and a publisher that knows only the range leaves the temperature alone");
    expect(!feed.takeChange(), "the change is sent once");

    feed.apply(range_only);
    expect(!feed.takeChange(), "the same value again is not a change");

    VehicleStatus warning;
    warning.range_warning = true;
    feed.apply(warning);
    const auto warned = feed.takeChange();
    expect(warned && warned->range_warning == true && warned->range_km == 380,
           "a new key joins the ones already known");

    feed.subscribed();
    expect(feed.takeChange().has_value(), "a fresh subscription is sent the current values again");

    if (failures == 0)
    {
        std::printf("all vehicle status feed checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
