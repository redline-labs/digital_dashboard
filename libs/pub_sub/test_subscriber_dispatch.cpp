// SPDX-License-Identifier: GPL-3.0-or-later
//
// What pub_sub promises about WHEN a subscriber's handler runs, over a real
// session. zenoh itself promises less: a closure may be entered by several
// threads at once, and a local put() delivers synchronously on the publishing
// thread. Two publishers on two threads is the deterministic way to get the
// first; a handler that publishes to its own key is the way to get the second.
#include "pub_sub/expression_subscriber.h"
#include "pub_sub/session_manager.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "can_frame.capnp.h"
#include "engine_rpm.capnp.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

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

template <typename Pred>
bool waitFor(std::chrono::milliseconds limit, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

std::string key(const char* leaf)
{
    return "test/subscriber_dispatch/" + std::to_string(::getpid()) + "/" + leaf;
}

void neverEnteredConcurrently()
{
    constexpr int kPerThread = 200;
    std::atomic<int> in_flight{0};
    std::atomic<int> most{0};
    std::atomic<int> received{0};

    pub_sub::ZenohTypedSubscriber<CanFrame> sub(key("concurrent"), [&](CanFrame::Reader) {
        const int now = ++in_flight;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now))
        {
        }
        // Wide enough that two unserialised deliveries overlap every time.
        std::this_thread::sleep_for(100us);
        --in_flight;
        ++received;
    });

    // One publisher per thread: a publisher's builder is not shareable.
    auto publish = [](int count) {
        pub_sub::ZenohPublisher<CanFrame> pub(key("concurrent"));
        for (int i = 0; i < count; ++i)
        {
            pub.fields().setId(static_cast<uint32_t>(i));
            pub.put();
        }
    };
    std::thread a(publish, kPerThread);
    std::thread b(publish, kPerThread);
    a.join();
    b.join();

    waitFor(3s, [&]() { return received.load() == 2 * kPerThread; });
    expect(received.load() == 2 * kPerThread, "every sample from both publishers arrives");
    expect(most.load() == 1, "a handler is never entered by two threads at once (saw " +
                                 std::to_string(most.load()) + ")");
}

void reentryOnTheSameThreadDoesNotDeadlock()
{
    std::atomic<int> received{0};
    pub_sub::ZenohPublisher<CanFrame> pub(key("reentrant"));
    std::mutex pub_mutex;

    pub_sub::ZenohTypedSubscriber<CanFrame> sub(key("reentrant"), [&](CanFrame::Reader frame) {
        ++received;
        if (frame.getId() == 1u)
        {
            // Delivered synchronously, into this same handler, on this thread.
            const std::lock_guard<std::mutex> lock(pub_mutex);
            pub.fields().setId(2u);
            pub.put();
        }
    });

    auto done = std::async(std::launch::async, [&]() {
        {
            const std::lock_guard<std::mutex> lock(pub_mutex);
            pub.fields().setId(1u);
        }
        // Not under pub_mutex: the handler takes it, and this put() may deliver
        // on this thread.
        pub.put();
        return waitFor(3s, [&]() { return received.load() == 2; });
    });
    if (done.wait_for(5s) != std::future_status::ready)
    {
        std::fprintf(stderr, "FAIL: a handler publishing to its own key deadlocked\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    expect(done.get(), "the handler's own publish comes back to it");
}

void secondRawCallbackIsIgnored()
{
    pub_sub::ZenohExpressionSubscriber sub(pub_sub::schema_type_t::EngineRpm, "rpm",
                                           key("expression"));
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    sub.setResultCallback<double>([&](double) { ++first; });
    sub.setResultCallback<double>([&](double) { ++second; });

    pub_sub::ZenohPublisher<EngineRpm> pub(key("expression"));
    waitFor(3s, [&]() {
        pub.fields().setRpm(1000);
        pub.put();
        return first.load() > 0;
    });
    expect(first.load() > 0, "the first callback receives samples");
    expect(second.load() == 0, "a second result callback does not replace the first");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    if (!pub_sub::SessionManager::getOrCreate())
    {
        std::fprintf(stderr, "SKIP: no zenoh session could be opened on this host\n");
        return PROJECT_TEST_SKIP_CODE;
    }

    neverEnteredConcurrently();
    reentryOnTheSameThreadDoesNotDeadlock();
    secondRawCallbackIsIgnored();

    std::fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
