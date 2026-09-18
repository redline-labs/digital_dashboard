// SPDX-License-Identifier: GPL-3.0-or-later
//
// waitForInterrupt(): ticks at its period, and returns promptly on a signal
// whichever thread takes it.
//
// Each case runs in a forked child, because the interrupted flag is
// process-wide and sticky -- the first case to raise a signal would otherwise
// decide every case after it.
//
// Mutation-check: drop the write() from the handler and the other-thread case
// sleeps its whole 10 s period and fails.

#include "cli/interrupt.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace
{

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// Runs `body` in a child; the child's exit code is the verdict.
template <typename Body>
void inChild(const char* what, Body body)
{
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        ::_exit(body() ? 0 : 1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
}

// The case the pipe exists for: the signal is delivered to a thread that is not
// the one waiting, as it is when a zenoh thread takes it.
bool signalOnAnotherThreadWakesTheWait()
{
    cli::installInterruptHandler();
    int ticks = 0;
    std::thread other([] {
        std::this_thread::sleep_for(100ms);
        std::raise(SIGTERM);  // raise() signals the calling thread
    });
    const auto start = Clock::now();
    cli::waitForInterrupt([&] { ++ticks; }, 10s);
    const auto elapsed = Clock::now() - start;
    other.join();
    return cli::interrupted() && ticks == 1 && elapsed < 2s;
}

bool ticksAtThePeriod()
{
    int ticks = 0;
    const auto start = Clock::now();
    cli::waitForInterrupt(
        [&] {
            if (++ticks == 5)
            {
                std::raise(SIGINT);
            }
        },
        30ms);
    const auto elapsed = Clock::now() - start;
    // Four sleeps between five ticks.
    return ticks == 5 && elapsed >= 110ms && elapsed < 2s;
}

bool anEarlierSignalReturnsAtOnce()
{
    cli::installInterruptHandler();
    std::raise(SIGTERM);
    int ticks = 0;
    const auto start = Clock::now();
    cli::waitForInterrupt([&] { ++ticks; }, 10s);
    return ticks == 0 && Clock::now() - start < 1s;
}

bool anEmptyTickIsAllowed()
{
    std::thread other([] {
        std::this_thread::sleep_for(50ms);
        std::raise(SIGINT);
    });
    cli::waitForInterrupt({}, 10s);
    other.join();
    return cli::interrupted();
}

}  // namespace

int main()
{
    inChild("a signal taken by another thread wakes the wait at once", signalOnAnotherThreadWakesTheWait);
    inChild("tick runs once per period until the signal", ticksAtThePeriod);
    inChild("a signal before the wait returns without ticking", anEarlierSignalReturnsAtOnce);
    inChild("an empty tick is allowed", anEmptyTickIsAllowed);

    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
