// SPDX-License-Identifier: GPL-3.0-or-later
//
// Backoff: failures step along the schedule, and only a connection that stayed
// up starts it over. The BD992 client reset on every connect, so a peer that
// accepted and closed at once was reconnected with no wait at all.

#include "byte_stream/backoff.h"

#include <cstdio>
#include <string>

namespace
{

using namespace std::chrono_literals;
using byte_stream::Backoff;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main()
{
    const auto t0 = Backoff::Clock::now();

    Backoff failing({100ms, 500ms, 2000ms});
    expect(failing.failed(t0) == 100ms, "the first failure waits the first entry");
    expect(failing.failed(t0) == 500ms, "the second the next");
    expect(failing.failed(t0) == 2000ms, "the third the last");
    expect(failing.failed(t0) == 2000ms, "and it stays on the last");

    // Accepts, then closes 10 ms later, every time.
    Backoff flapping({100ms, 500ms, 2000ms}, 10s);
    flapping.connected(t0);
    expect(flapping.failed(t0 + 10ms) == 100ms, "a connection that dropped at once waits");
    flapping.connected(t0 + 200ms);
    expect(flapping.failed(t0 + 210ms) == 500ms,
           "and the next quick drop waits longer: connecting is not success");

    Backoff stable({100ms, 500ms, 2000ms}, 10s);
    stable.failed(t0);
    stable.failed(t0);
    stable.connected(t0);
    expect(stable.failed(t0 + 11s) == 100ms, "a connection that stayed up starts the schedule over");

    Backoff none({});
    expect(none.failed(t0) == 0ms, "an empty schedule retries at once");

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
