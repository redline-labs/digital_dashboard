// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hand-off between the thread that sends a configuration command and the
// thread that decodes the relay's answer.
//
// This is tested at all because the node cannot be: the interesting cases are
// orderings between two threads, and inside a zenoh service callback they can
// be reasoned about but not provoked. Every one below is a real sequence the
// device produces.
//
// The one that is not obvious is the STALE answer. These acknowledgements
// arrive late by nature -- seconds after the command, and only when a human was
// holding a switch at the right moment -- so an answer turning up after its
// caller gave up is not a corner case, it is what happens whenever someone
// mistimes the press and tries again. Handing that answer to the next command
// would report the wrong command as accepted.

#include "msel/response_waiter.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace std::chrono_literals;

// An answer that arrives while the sender is already waiting: the ordinary
// success path, and the only one that needs two threads to produce.
void test_an_answer_wakes_the_waiter()
{
    msel::ResponseWaiter waiter;
    waiter.arm();

    std::thread relay([&waiter] {
        std::this_thread::sleep_for(20ms);
        waiter.deliver(msel::ConfigResponse::Success);
    });

    const auto started = std::chrono::steady_clock::now();
    const auto answer = waiter.wait(5s);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    relay.join();

    check(answer.has_value() && *answer == msel::ConfigResponse::Success,
          "the answer reaches the waiting thread");

    // The point of a condition variable rather than a sleep-and-poll: it
    // returns when the answer lands, not when the window closes.
    check(elapsed < 2s, "and wakes it immediately rather than at the timeout");
    check(!waiter.armed(), "waiting disarms");
}

// The race the arming order exists to close. On a fast bus the answer can be
// decoded before the sending thread gets back from its publish.
void test_an_answer_that_beats_the_waiter()
{
    msel::ResponseWaiter waiter;
    waiter.arm();
    waiter.deliver(msel::ConfigResponse::IdMismatch);

    const auto started = std::chrono::steady_clock::now();
    const auto answer = waiter.wait(5s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(answer.has_value() && *answer == msel::ConfigResponse::IdMismatch,
          "an answer delivered before the wait is still returned");
    check(elapsed < 2s, "without waiting out the window for something already in hand");
}

// Silence is the ORDINARY outcome: the relay ignores a configuration command
// outright unless the external kill switch is held, and an ignored command is
// not answered at all.
void test_silence_times_out_and_says_so()
{
    msel::ResponseWaiter waiter;
    waiter.arm();

    const auto started = std::chrono::steady_clock::now();
    const auto answer = waiter.wait(60ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(!answer.has_value(), "no answer is reported as no answer");
    check(elapsed >= 60ms, "after waiting the whole window");
    check(!waiter.armed(), "and the waiter is disarmed afterwards");
}

// THE ONE THAT MATTERS. An answer to a command whose caller has already given
// up must not be handed to the next command.
void test_a_late_answer_is_not_given_to_the_next_command()
{
    msel::ResponseWaiter waiter;

    // First command: sent, never answered in time.
    waiter.arm();
    check(!waiter.wait(20ms).has_value(), "the first command times out");

    // Its answer turns up afterwards, as these do.
    waiter.deliver(msel::ConfigResponse::Success);

    // Second command, sent by someone who has just been told the first failed.
    waiter.arm();
    const auto answer = waiter.wait(40ms);

    check(!answer.has_value(),
          "the late answer to the FIRST command is not reported as the second's -- which would "
          "tell a caller a setting had been accepted when nothing had answered it");
}

// A frame that arrives when no command is outstanding at all. Ordinary on a
// shared bus, and it must not be stored for whatever is sent next.
void test_an_unsolicited_answer_is_dropped()
{
    msel::ResponseWaiter waiter;

    waiter.deliver(msel::ConfigResponse::Success);
    check(!waiter.armed(), "delivering to nothing arms nothing");

    waiter.arm();
    check(!waiter.wait(20ms).has_value(), "and is not waiting in the slot for the next command");
}

// A command that was never transmitted -- refused locally, or a frame that
// would not build -- has no answer coming.
void test_disarming_clears_the_slot()
{
    msel::ResponseWaiter waiter;

    waiter.arm();
    waiter.disarm();
    check(!waiter.armed(), "disarm disarms");

    waiter.deliver(msel::ConfigResponse::Success);
    waiter.arm();
    check(!waiter.wait(20ms).has_value(),
          "an answer delivered after disarming is not kept for the next command");
}

// Under the node's command mutex these run strictly one after another. This is
// the same sequence, checked to make sure the slot is genuinely reusable rather
// than working once.
void test_consecutive_commands_each_get_their_own_answer()
{
    msel::ResponseWaiter waiter;

    for (const auto expected : { msel::ConfigResponse::Success, msel::ConfigResponse::InvalidId,
                                 msel::ConfigResponse::FrameCheckError })
    {
        waiter.arm();

        std::thread relay([&waiter, expected] {
            std::this_thread::sleep_for(5ms);
            waiter.deliver(expected);
        });

        const auto answer = waiter.wait(5s);
        relay.join();

        check(answer.has_value() && *answer == expected,
              "each command in turn gets its own answer");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_answer_wakes_the_waiter();
    test_an_answer_that_beats_the_waiter();
    test_silence_times_out_and_says_so();
    test_a_late_answer_is_not_given_to_the_next_command();
    test_an_unsolicited_answer_is_dropped();
    test_disarming_clears_the_slot();
    test_consecutive_commands_each_get_their_own_answer();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all MSEL response waiter checks passed");
    return 0;
}
