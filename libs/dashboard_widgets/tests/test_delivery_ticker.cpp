// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared delivery ticker, against fake targets and no bus.
//
// What it is for, each pinned: nothing runs while nothing arrives; a burst from
// another thread becomes one pass, not one per sample; no sample is left in a
// mailbox when a wake races the tick; a binding goes stale at its deadline
// without anything polling for it; and a target may be removed from inside a
// delivery.
#include "dashboard/delivery_ticker.h"
#include "dashboard/staleness.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using dashboard::DeliveryTicker;
using dashboard::StalenessTracker;

namespace
{

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

void pump(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
}

// The shape of ExpressionSubscription without zenoh: a one-slot mailbox
// written from any thread, and a staleness tracker read on the GUI thread.
class FakeTarget final : public dashboard::DeliveryTarget
{
  public:
    FakeTarget(DeliveryTicker& ticker, std::chrono::milliseconds stale_after)
        : ticker_(ticker)
        , staleness_(stale_after, clock::now())
    {
        ticker_.add(this);
    }
    ~FakeTarget() override { ticker_.remove(this); }

    void post(int value)  // any thread
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_ = value;
        }
        ticker_.wake();
    }

    void drain(clock::time_point now) override
    {
        ++passes;
        std::optional<int> value;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            value.swap(pending_);
        }
        if (value)
        {
            ++deliveries;
            last = *value;
            staleness_.onSample(now);
        }
        if (staleness_.poll(now) == StalenessTracker::Edge::became_stale)
        {
            went_stale_at = now;
        }
        if (on_drain)
        {
            on_drain();
        }
    }

    std::optional<clock::time_point> staleDeadline() const override { return staleness_.deadline(); }

    int passes = 0;
    int deliveries = 0;
    int last = -1;
    std::optional<clock::time_point> went_stale_at;
    std::function<void()> on_drain;

  private:
    DeliveryTicker& ticker_;
    std::mutex mutex_;
    std::optional<int> pending_;
    StalenessTracker staleness_;
};

void testIdleRunsNothing()
{
    DeliveryTicker ticker;
    FakeTarget target(ticker, 0ms);
    pump(100ms);
    check(target.passes == 0, "with nothing arriving and no timeout, no tick runs at all");
    check(!ticker.frameActive() && !ticker.staleTimerActive(), "and no timer is armed");
}

void testBurstIsOnePass()
{
    DeliveryTicker ticker;
    FakeTarget a(ticker, 0ms);
    FakeTarget b(ticker, 0ms);
    FakeTarget c(ticker, 0ms);

    std::thread producer([&]() {
        for (int i = 0; i < 1000; ++i)
        {
            a.post(i);
            b.post(i);
            c.post(i);
        }
    });
    producer.join();
    pump(100ms);

    check(a.last == 999 && b.last == 999 && c.last == 999, "every target ends on the newest value");
    check(a.passes == 1 && b.passes == 1 && c.passes == 1,
          "and 3000 samples from another thread are ONE pass, not one per sample or per target");
    check(!ticker.frameActive(), "after which the frame timer stops");
}

void testNoSampleIsStranded()
{
    // Wakes racing the tick's disarm. Whatever the interleaving, the last value
    // written must be delivered: a lost wake strands it in the mailbox until
    // the NEXT sample, which on a slow topic is a gauge showing old data.
    DeliveryTicker ticker;
    FakeTarget target(ticker, 0ms);
    std::atomic<bool> stop{false};
    std::atomic<int> written{-1};
    std::thread producer([&]() {
        for (int i = 0; !stop.load(); ++i)
        {
            target.post(i);
            written.store(i);
            std::this_thread::sleep_for(std::chrono::microseconds(100 + (i * 37) % 900));
        }
    });
    pump(400ms);
    stop.store(true);
    producer.join();
    pump(60ms);
    check(target.last == written.load(), "the last sample of a racing stream is delivered");
    const int rate_limited = target.deliveries;
    check(rate_limited <= 400 / 16 + 3, "at no more than one delivery per frame (" + std::to_string(rate_limited) + ")");
}

void testSampleDuringDrainIsNotStranded()
{
    // The exact interleaving the disarm order exists for, made deterministic:
    // a sample lands after this pass has emptied the mailbox. It must get a
    // frame of its own; if the tick disarmed AFTER draining, that wake would
    // be swallowed and the value would sit there until the next sample.
    DeliveryTicker ticker;
    FakeTarget target(ticker, 0ms);
    bool once = true;
    target.on_drain = [&]() {
        if (once)
        {
            once = false;
            target.post(2);
        }
    };
    target.post(1);
    pump(80ms);
    check(target.last == 2, "a sample arriving mid-pass is delivered on the next frame");
}

void testStaleAtDeadlineWithoutPolling()
{
    DeliveryTicker ticker;
    const auto armed = std::chrono::steady_clock::now();
    FakeTarget target(ticker, 120ms);
    check(ticker.staleTimerActive(), "a binding with a timeout arms the stale timer");
    pump(250ms);
    check(target.went_stale_at.has_value(), "a silent binding goes stale");
    if (target.went_stale_at)
    {
        const auto late = *target.went_stale_at - armed;
        check(late >= 120ms, "not before its timeout");
        check(late < 120ms + 50ms, "and promptly after it");
    }
    check(target.passes <= 3, "with a handful of wakeups, not one per frame (" + std::to_string(target.passes) + ")");
    check(!ticker.staleTimerActive(), "and once stale, nothing is left to wait for");

    target.post(1);
    pump(40ms);
    check(ticker.staleTimerActive(), "a sample re-arms it");
}

void testRemoveDuringDelivery()
{
    DeliveryTicker ticker;
    FakeTarget first(ticker, 0ms);
    auto second = std::make_unique<FakeTarget>(ticker, 0ms);
    first.on_drain = [&]() { second.reset(); };  // a widget torn down by another's update
    first.post(1);
    pump(50ms);
    check(!second, "a target removed mid-pass");
    check(ticker.targetCount() == 1, "leaves the list compacted");
    first.on_drain = nullptr;
    first.post(2);
    pump(50ms);
    check(first.last == 2, "and the ticker keeps going");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testIdleRunsNothing();
    testBurstIsOnePass();
    testNoSampleIsStranded();
    testSampleDuringDrainIsNotStranded();
    testStaleAtDeadlineWithoutPolling();
    testRemoveDuringDelivery();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
