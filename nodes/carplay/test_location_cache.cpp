// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocationCache: a GPS source that goes quiet must stop looking like a fix.
#include "location_cache.h"

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

using carplay::LocationCache;
using carplay::LocationFix;
using namespace std::chrono_literals;

}  // namespace

int main()
{
    const LocationCache::steady::time_point t0{};
    const LocationCache::system::time_point wall0{1790000000s};

    LocationCache cache(3000ms);
    expect(!cache.current(t0).has_value(), "nothing is sent before a source has published");

    LocationFix fix;
    fix.latitude_deg = 48.78;
    fix.longitude_deg = 9.18;
    fix.utc_epoch_ms = 0;  // the source had no time of its own
    cache.update(fix, t0, wall0);

    const auto fresh = cache.current(t0 + 1s);
    expect(fresh && fresh->valid && fresh->latitude_deg == 48.78, "a fresh fix goes out as a fix");
    expect(fresh && fresh->utc_epoch_ms == 1790000000000u,
           "stamped with when it arrived, not when it is sent");

    const auto stale = cache.current(t0 + 4s);
    expect(stale && !stale->valid, "a fix older than the limit goes out as no fix");
    expect(stale && stale->utc_epoch_ms == 1790000000000u, "and still says when it was taken");

    LocationFix timed = fix;
    timed.utc_epoch_ms = 1790000123456u;
    cache.update(timed, t0 + 5s, wall0 + 5s);
    const auto again = cache.current(t0 + 6s);
    expect(again && again->valid, "a new fix makes it valid again");
    expect(again && again->utc_epoch_ms == 1790000123456u, "keeping the source's own time");

    LocationFix nofix = fix;
    nofix.valid = false;
    cache.update(nofix, t0 + 7s, wall0 + 7s);
    const auto source_says_no = cache.current(t0 + 7s);
    expect(source_says_no && !source_says_no->valid, "a source reporting no fix is passed on");

    LocationCache bench(3000ms);
    bench.pin(fix);
    const auto pinned = bench.current(t0 + 1h);
    expect(pinned && pinned->valid, "a pinned bench fix never ages");

    if (failures == 0)
    {
        std::printf("all location cache checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
