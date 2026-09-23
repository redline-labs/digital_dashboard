// SPDX-License-Identifier: GPL-3.0-or-later
//
// HandlerSlot: detaching is a barrier. The failure it exists to prevent is a
// handler that is still running, or about to run, after set(nullptr) returned
// and the pipeline destroyed what it captured -- a use-after-free with no
// symptom until it lands on reused memory.
#include "handler_slot.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

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

using namespace std::chrono_literals;

}  // namespace

int main()
{
    {
        carplay::HandlerSlot<int> slot;
        slot(1);  // empty: a no-op, not a bad_function_call
        int seen = 0;
        slot.set([&seen](int v) { seen = v; });
        slot(7);
        expect(seen == 7, "an installed handler is called with the argument");
        slot.set(nullptr);
        slot(9);
        expect(seen == 7, "a detached handler is not called");
    }

    {
        // A call in flight on another thread: set(nullptr) must wait for it.
        carplay::HandlerSlot<> slot;
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        std::atomic<bool> finished{false};
        slot.set([&]() {
            entered.set_value();
            released.wait();
            finished = true;
        });

        std::thread caller([&slot]() { slot(); });
        entered.get_future().wait();

        std::atomic<bool> detached{false};
        std::thread detacher([&]() {
            slot.set(nullptr);
            detached = true;
        });

        std::this_thread::sleep_for(100ms);
        expect(!detached, "set(nullptr) does not return while the old handler is running");
        release.set_value();
        detacher.join();
        caller.join();
        expect(finished, "the in-flight call ran to completion before detach returned");
    }

    {
        // Arguments pass by reference where the signature says so.
        carplay::HandlerSlot<const std::string&> slot;
        const std::string* got = nullptr;
        slot.set([&got](const std::string& s) { got = &s; });
        const std::string value = "x";
        slot(value);
        expect(got == &value, "a const& argument is not copied");
    }

    std::fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
