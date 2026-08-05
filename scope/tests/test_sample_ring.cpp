// SPDX-License-Identifier: GPL-3.0-or-later
//
// The buffering layer: the lock-free hand-off from the thread samples arrive
// on, the retained history the panel draws, and the retention limits.
//
// This is the part of scope that has to be right for the plot to mean anything.
// A ring that loses a sample silently draws a line that never happened; one
// that mis-orders on wraparound draws it backwards; one whose binary search is
// off by one clips the leftmost point of every frame. None of those look like
// bugs on screen -- they look like data.
//
// No zenoh and no Qt here, so it runs everywhere and cannot skip itself.

#include "scope/sample_ring.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// ------------------------------------------------------------------ StagingRing

void testAnEmptyRingDrainsNothing()
{
    scope::StagingRing ring(8);
    std::vector<scope::Sample> out;
    expect(ring.drain(out) == 0, "an empty ring drains nothing");
    expect(out.empty(), "an empty ring appends nothing to the output");
    expect(ring.dropped() == 0, "an empty ring has dropped nothing");
}

void testSamplesComeBackInOrder()
{
    scope::StagingRing ring(8);
    for (int i = 0; i < 5; ++i)
    {
        expect(ring.push({static_cast<double>(i), static_cast<double>(i * 10)}),
               "a push into a ring with room succeeds");
    }

    std::vector<scope::Sample> out;
    expect(ring.drain(out) == 5, "every pushed sample is drained");

    bool ordered = out.size() == 5;
    for (std::size_t i = 0; ordered && i < out.size(); ++i)
    {
        ordered = out[i].t == static_cast<double>(i) && out[i].v == static_cast<double>(i * 10);
    }
    expect(ordered, "samples come back in the order they were pushed");
}

void testTheRingHoldsItsFullCapacity()
{
    // The implementation keeps one slot empty to tell full from empty apart, so
    // this is exactly the off-by-one that would make a ring hold capacity-1.
    scope::StagingRing ring(4);
    for (int i = 0; i < 4; ++i)
    {
        expect(ring.push({static_cast<double>(i), 0.0}),
               "a ring accepts samples up to its stated capacity");
    }
    expect(ring.dropped() == 0, "filling a ring exactly drops nothing");
}

void testOverflowIsCountedRatherThanSilent()
{
    scope::StagingRing ring(4);
    for (int i = 0; i < 4; ++i)
    {
        ring.push({static_cast<double>(i), 0.0});
    }

    expect(!ring.push({99.0, 99.0}), "a push into a full ring reports failure");
    expect(ring.dropped() == 1, "a dropped sample is counted");

    ring.push({100.0, 0.0});
    ring.push({101.0, 0.0});
    expect(ring.dropped() == 3, "every dropped sample is counted");

    // The drops must not have corrupted what was already there.
    std::vector<scope::Sample> out;
    expect(ring.drain(out) == 4, "overflow does not disturb the samples already held");
    expect(out.size() == 4 && out[0].t == 0.0 && out[3].t == 3.0,
           "the samples held are the ones pushed before the ring filled");
}

void testTheRingWrapsCorrectly()
{
    // Push and drain far more than capacity, so the indices wrap many times.
    scope::StagingRing ring(4);
    std::vector<scope::Sample> out;

    double expected_next = 0.0;
    for (int round = 0; round < 100; ++round)
    {
        for (int i = 0; i < 3; ++i)
        {
            ring.push({static_cast<double>(round * 3 + i), 0.0});
        }

        out.clear();
        ring.drain(out);
        for (const scope::Sample& sample : out)
        {
            if (sample.t != expected_next)
            {
                expect(false, "samples stay in order across many wraps of the ring");
                return;
            }
            expected_next += 1.0;
        }
    }

    expect(expected_next == 300.0, "every sample survives 100 rounds of wraparound");
    expect(ring.dropped() == 0, "a ring drained every round never overflows");
}

void testAProducerAndConsumerOnDifferentThreadsLoseNothing()
{
    // The whole reason this class exists is that the producer is a zenoh RX
    // thread and the consumer is the GUI thread. A single-threaded test would
    // never exercise the release/acquire pairing that makes that safe.
    constexpr int kTotal = 200000;
    scope::StagingRing ring(1024);

    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < kTotal; ++i)
        {
            // Wait for room via full() rather than by retrying push(), so this
            // test is about ordering and visibility rather than about the drop
            // path (covered above). Retrying push() would also count a drop per
            // attempt, which is correct for the real producer -- a zenoh
            // callback pushes once and moves on -- but not for a caller that
            // waits.
            while (ring.full())
            {
                std::this_thread::yield();
            }
            ring.push({static_cast<double>(i), static_cast<double>(i) * 2.0});
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::vector<scope::Sample> out;
    double expected_next = 0.0;
    bool ordered = true;
    int received = 0;

    while (received < kTotal)
    {
        out.clear();
        ring.drain(out);
        for (const scope::Sample& sample : out)
        {
            if (sample.t != expected_next || sample.v != expected_next * 2.0)
            {
                ordered = false;
            }
            expected_next += 1.0;
            ++received;
        }
        if (out.empty() && producer_done.load(std::memory_order_acquire) && received < kTotal)
        {
            // Producer finished and the ring is empty but we are short: samples
            // went missing, which is the failure this test exists to catch.
            break;
        }
    }

    producer.join();

    expect(received == kTotal, "no sample is lost between two threads");
    expect(ordered, "every sample crosses the thread boundary intact and in order");
    expect(ring.dropped() == 0, "a producer that waits for room drops nothing");
}

// ---------------------------------------------------------------- SampleHistory

void testHistoryStartsEmpty()
{
    scope::SampleHistory history(16);
    expect(history.empty(), "a new history is empty");
    expect(history.size() == 0, "a new history has no samples");
    expect(history.lowerBound(0.0) == 0, "lowerBound on an empty history is 0");
}

void testHistoryIndexesOldestFirst()
{
    scope::SampleHistory history(16);
    for (int i = 0; i < 5; ++i)
    {
        history.append({static_cast<double>(i), static_cast<double>(i)});
    }

    expect(history.size() == 5, "appended samples are all retained while there is room");
    expect(history[0].t == 0.0, "index 0 is the oldest sample");
    expect(history[4].t == 4.0, "the last index is the newest sample");
    expect(history.oldest().t == 0.0, "oldest() agrees with index 0");
    expect(history.newest().t == 4.0, "newest() agrees with the last index");
}

void testHistoryOverwritesOldestWhenFull()
{
    // Unlike the staging ring, this side should keep the NEWEST data: a plot
    // that stopped updating its right edge under load would look like a hang.
    scope::SampleHistory history(4);
    for (int i = 0; i < 10; ++i)
    {
        history.append({static_cast<double>(i), 0.0});
    }

    expect(history.size() == 4, "a full history stays at its capacity");
    expect(history.oldest().t == 6.0, "the oldest retained sample is the right one after wrapping");
    expect(history.newest().t == 9.0, "the newest sample is always retained");
}

void testLowerBoundFindsTheFirstSampleInTheWindow()
{
    scope::SampleHistory history(16);
    for (int i = 0; i < 10; ++i)
    {
        history.append({static_cast<double>(i) * 2.0, 0.0});  // t = 0, 2, 4, ... 18
    }

    expect(history.lowerBound(-1.0) == 0, "a bound before every sample selects all of them");
    expect(history.lowerBound(0.0) == 0, "a bound exactly on the first sample includes it");
    expect(history.lowerBound(4.0) == 2, "a bound exactly on a sample includes that sample");
    expect(history.lowerBound(5.0) == 3, "a bound between samples selects the next one");
    expect(history.lowerBound(18.0) == 9, "a bound on the last sample includes it");
    expect(history.lowerBound(19.0) == 10,
           "a bound after every sample selects none, reported as size()");
}

void testLowerBoundIsCorrectAfterWrapping()
{
    // The search runs over logical indices while the storage has wrapped, which
    // is where an off-by-one would hide.
    scope::SampleHistory history(4);
    for (int i = 0; i < 10; ++i)
    {
        history.append({static_cast<double>(i), 0.0});  // Retains t = 6, 7, 8, 9.
    }

    expect(history.lowerBound(6.0) == 0, "after wrapping, a bound on the oldest sample includes it");
    expect(history.lowerBound(8.0) == 2, "after wrapping, the search lands on the right index");
    expect(history.lowerBound(9.5) == 4, "after wrapping, a bound past the end reports size()");
}

void testTrimDropsOnlyWhatIsOlder()
{
    scope::SampleHistory history(16);
    for (int i = 0; i < 10; ++i)
    {
        history.append({static_cast<double>(i), 0.0});
    }

    history.trimOlderThan(4.0);
    expect(history.size() == 6, "trimming drops exactly the samples older than the bound");
    expect(history.oldest().t == 4.0, "a sample exactly on the bound is kept, not dropped");
    expect(history.newest().t == 9.0, "trimming leaves the newest samples alone");

    history.trimOlderThan(-100.0);
    expect(history.size() == 6, "trimming to before the data drops nothing");

    history.trimOlderThan(1000.0);
    expect(history.size() == 0, "trimming past all the data empties the history");
    expect(history.empty(), "an emptied history reports itself empty");
}

void testSingleSampleEdgeCases()
{
    scope::SampleHistory history(4);
    history.append({5.0, 1.0});

    expect(history.size() == 1, "a history with one sample has size 1");
    expect(history.oldest().t == 5.0 && history.newest().t == 5.0,
           "with one sample, oldest and newest are the same");
    expect(history.lowerBound(5.0) == 0, "lowerBound finds a lone sample at its own time");
    expect(history.lowerBound(5.1) == 1, "lowerBound past a lone sample reports size()");
}

// ----------------------------------------------------------------- SignalBuffer

void testBufferMovesStagedSamplesIntoHistory()
{
    scope::SignalBuffer buffer(/*history_seconds=*/60.0, /*max_points=*/1000,
                               /*staging_capacity=*/64);

    expect(buffer.history().empty(), "a new buffer has no history");
    expect(buffer.received() == 0, "a new buffer has received nothing");

    for (int i = 0; i < 10; ++i)
    {
        buffer.push({static_cast<double>(i), static_cast<double>(i)});
    }

    expect(buffer.history().empty(), "pushed samples are not visible before a drain");

    buffer.drain(/*now=*/10.0);
    expect(buffer.history().size() == 10, "draining moves staged samples into the history");
    expect(buffer.received() == 10, "the received count follows what reached the history");
}

void testBufferTrimsToTheTimeWindow()
{
    scope::SignalBuffer buffer(/*history_seconds=*/5.0, /*max_points=*/1000,
                               /*staging_capacity=*/64);

    for (int i = 0; i < 20; ++i)
    {
        buffer.push({static_cast<double>(i), 0.0});  // t = 0 .. 19
    }
    buffer.drain(/*now=*/19.0);

    // Retention is 5 s back from now, so t >= 14.
    expect(buffer.history().size() == 6, "the history is trimmed to the retention window");
    expect(buffer.history().oldest().t == 14.0, "trimming keeps exactly the window asked for");
    expect(buffer.received() == 20, "the received count is not reduced by trimming");
}

void testBufferHonoursThePointCap()
{
    // The point cap is what stops a fast publisher growing this without bound
    // even when the time window would allow it.
    scope::SignalBuffer buffer(/*history_seconds=*/1000.0, /*max_points=*/8,
                               /*staging_capacity=*/64);

    for (int i = 0; i < 40; ++i)
    {
        buffer.push({static_cast<double>(i), 0.0});
    }
    buffer.drain(/*now=*/40.0);

    expect(buffer.history().size() == 8, "the point cap bounds the history whatever the window");
    expect(buffer.history().newest().t == 39.0, "the point cap keeps the newest samples");
}

void testBufferReportsDrops()
{
    scope::SignalBuffer buffer(/*history_seconds=*/60.0, /*max_points=*/1000,
                               /*staging_capacity=*/4);

    for (int i = 0; i < 10; ++i)
    {
        buffer.push({static_cast<double>(i), 0.0});
    }

    expect(buffer.dropped() == 6, "samples dropped before a drain are reported, not hidden");

    buffer.drain(/*now=*/10.0);
    expect(buffer.history().size() == 4, "only the samples that fit the staging ring arrive");
    expect(buffer.dropped() == 6, "the drop count survives a drain");
}

void testDrainingRepeatedlyIsHarmless()
{
    scope::SignalBuffer buffer(/*history_seconds=*/60.0, /*max_points=*/1000,
                               /*staging_capacity=*/64);
    buffer.push({1.0, 1.0});
    buffer.drain(1.0);
    buffer.drain(1.0);
    buffer.drain(1.0);

    expect(buffer.history().size() == 1, "draining an already-drained buffer adds nothing");
    expect(buffer.received() == 1, "draining an already-drained buffer does not inflate the count");
}

}  // namespace

int main()
{
    testAnEmptyRingDrainsNothing();
    testSamplesComeBackInOrder();
    testTheRingHoldsItsFullCapacity();
    testOverflowIsCountedRatherThanSilent();
    testTheRingWrapsCorrectly();
    testAProducerAndConsumerOnDifferentThreadsLoseNothing();

    testHistoryStartsEmpty();
    testHistoryIndexesOldestFirst();
    testHistoryOverwritesOldestWhenFull();
    testLowerBoundFindsTheFirstSampleInTheWindow();
    testLowerBoundIsCorrectAfterWrapping();
    testTrimDropsOnlyWhatIsOlder();
    testSingleSampleEdgeCases();

    testBufferMovesStagedSamplesIntoHistory();
    testBufferTrimsToTheTimeWindow();
    testBufferHonoursThePointCap();
    testBufferReportsDrops();
    testDrainingRepeatedlyIsHarmless();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
