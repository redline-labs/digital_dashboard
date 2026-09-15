// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-memory capture: what it keeps, what it throws away, and whether it
// says so.
//
// Both bounds are load-bearing and for different reasons. With CarPlay
// streaming, the bus runs about 1.5 GB/hour, so a capture that only honoured
// the time bound is OOM-killed within minutes -- taking the capture with it.
// Telemetry alone is around 11 MB/hour, where a byte bound would never bite and
// the time bound is the only thing keeping the session finite. A buffer that
// checked whichever bound its author expected to hit would fail on the other
// workload, silently, in production.
//
// Eviction accounting is equally load-bearing: a capture quietly dropping its
// head makes the start of a trace read as a publisher that had not started yet.
//
// No Qt and no zenoh, so this is a plain unit test. The threading case is real
// threads, because the producer genuinely is a zenoh RX thread.

#include "scope/capture_buffer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
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

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;  // a plausible 2026 timestamp

bag::QueuedMessage message(int index, std::size_t payload_bytes, std::uint64_t log_time_ns)
{
    bag::QueuedMessage message;
    message.key = "vehicle/engine/rpm";
    message.schema = "EngineRpm";
    message.payload.assign(payload_bytes, static_cast<std::uint8_t>(index & 0xFF));
    message.log_time_ns = log_time_ns;
    message.publish_time_ns = log_time_ns;
    return message;
}

std::size_t countRetained(const scope::CaptureBuffer& buffer)
{
    std::size_t seen = 0;
    buffer.forEach(0, std::numeric_limits<std::uint64_t>::max(),
                   [&seen](const bag::QueuedMessage&)
                   {
                       ++seen;
                       return true;
                   });
    return seen;
}

// ---------------------------------------------------------------- basic order

void testRetainsInOrder()
{
    scope::CaptureBuffer buffer(0, 0.0);  // Both bounds disabled.

    for (int i = 0; i < 100; ++i)
    {
        buffer.push(message(i, 16, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
    }

    expect(buffer.size() == 100, "everything is retained when neither bound applies");
    expect(countRetained(buffer) == 100, "and forEach visits all of it");

    bool ordered = true;
    std::uint64_t last = 0;
    buffer.forEach(0, std::numeric_limits<std::uint64_t>::max(),
                   [&](const bag::QueuedMessage& seen)
                   {
                       if (seen.log_time_ns < last)
                       {
                           ordered = false;
                       }
                       last = seen.log_time_ns;
                       return true;
                   });
    expect(ordered, "messages come back oldest first");

    expect(buffer.evicted() == 0, "nothing was evicted");
    expect(std::abs(buffer.retainedSpanSeconds() - 0.099) < 1e-6,
           "the retained span is the time between the oldest and the newest (" +
               std::to_string(buffer.retainedSpanSeconds()) + ")");
}

void testRangeQuery()
{
    scope::CaptureBuffer buffer(0, 0.0);
    for (int i = 0; i < 100; ++i)
    {
        buffer.push(message(i, 16, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
    }

    std::size_t in_range = 0;
    buffer.forEach(kBase + 10'000'000ull, kBase + 19'000'000ull,
                   [&in_range](const bag::QueuedMessage&)
                   {
                       ++in_range;
                       return true;
                   });
    expect(in_range == 10,
           "a range is closed at both ends, like BagReader's (" + std::to_string(in_range) + ")");

    std::size_t past_end = 0;
    buffer.forEach(kBase + 1'000'000'000ull, std::numeric_limits<std::uint64_t>::max(),
                   [&past_end](const bag::QueuedMessage&)
                   {
                       ++past_end;
                       return true;
                   });
    expect(past_end == 0, "a range past the end returns nothing, not everything");
}

// ------------------------------------------------------------------- density
//
// Checked against a brute-force reference over random data, for the same reason
// scope_test_decimate is: every way a histogram can be wrong looks like DATA. A
// bucket that collects its neighbour's messages draws a plausible shape, and the
// only symptom is that the busy part of a recording is in the wrong place --
// which is exactly what someone is using the strip to find.

void testDensityAgainstBruteForce()
{
    scope::CaptureBuffer buffer(0, 0.0);

    std::mt19937 rng(20260807);
    std::uniform_int_distribution<std::uint64_t> offset(0, 9'999'999'999ull);

    std::vector<std::uint64_t> times;
    for (int i = 0; i < 500; ++i)
    {
        times.push_back(kBase + offset(rng));
    }
    // The buffer assumes arrival order, and so does the early exit it shares
    // with forEach().
    std::sort(times.begin(), times.end());
    for (std::size_t i = 0; i < times.size(); ++i)
    {
        buffer.push(message(static_cast<int>(i), 16, times[i]));
    }

    for (const std::size_t buckets : {1u, 7u, 64u, 1000u})
    {
        const std::uint64_t t0 = kBase;
        const std::uint64_t t1 = kBase + 10'000'000'000ull;

        std::vector<std::uint32_t> got;
        buffer.density(t0, t1, buckets, got);
        expect(got.size() == buckets, "density fills exactly `buckets` entries");

        std::vector<std::uint32_t> want(buckets, 0);
        for (const std::uint64_t t : times)
        {
            if (t < t0 || t > t1)
            {
                continue;
            }
            std::size_t b = static_cast<std::size_t>(((t - t0) * buckets) / (t1 - t0));
            b = std::min(b, buckets - 1);
            ++want[b];
        }
        expect(got == want,
               "density matches a brute-force count over " + std::to_string(buckets) + " buckets");

        std::uint64_t total = 0;
        for (const std::uint32_t c : got)
        {
            total += c;
        }
        expect(total == times.size(), "and every message landed in exactly one bucket");
    }
}

void testDensityBoundaryConvention()
{
    scope::CaptureBuffer buffer(0, 0.0);

    // One message exactly on each edge, and one exactly on an internal boundary.
    buffer.push(message(0, 16, kBase));
    buffer.push(message(1, 16, kBase + 5'000'000ull));
    buffer.push(message(2, 16, kBase + 10'000'000ull));

    std::vector<std::uint32_t> got;
    buffer.density(kBase, kBase + 10'000'000ull, 2, got);

    // Bucket i covers [t0 + i*dt, t0 + (i+1)*dt), with the LAST closed at the
    // top -- the same convention decimateMinMax uses, so the strip and the plot
    // never disagree about which side of a boundary a message fell on.
    // Bucket 0 gets only t0; bucket 1 gets the message ON the boundary and the
    // one on t1.
    expect(got.size() == 2 && got[0] == 1 && got[1] == 2,
           "an internal boundary belongs to the later bucket, and t1 to the last one");
}

void testDensityDegenerateArguments()
{
    scope::CaptureBuffer buffer(0, 0.0);
    buffer.push(message(0, 16, kBase));

    std::vector<std::uint32_t> got{1, 2, 3};

    buffer.density(kBase, kBase, 4, got);
    expect(got.size() == 4 && got[0] == 0,
           "a zero-width range answers zeroes rather than dividing by it");

    buffer.density(kBase + 100, kBase, 4, got);
    expect(got.size() == 4, "an inverted range is survivable");

    buffer.density(kBase, kBase + 1000, 0, got);
    expect(got.empty(), "zero buckets asks for nothing and gets nothing");
}

void testDensityOnAnEmptyBuffer()
{
    scope::CaptureBuffer buffer(0, 0.0);
    std::vector<std::uint32_t> got;
    buffer.density(kBase, kBase + 1'000'000ull, 8, got);

    expect(got.size() == 8, "an empty buffer still fills the bucket vector");
    expect(std::all_of(got.begin(), got.end(), [](std::uint32_t c) { return c == 0; }),
           "with zeroes, so the strip draws a flat band rather than nothing at all");
}

// -------------------------------------------------------------- byte eviction

void testEvictsByBytes()
{
    // 100 bytes of payload plus the key ("vehicle/engine/rpm", 18) and schema
    // ("EngineRpm", 9): 127 bytes each. A 1000-byte cap holds 7.
    scope::CaptureBuffer buffer(1000, 0.0);

    for (int i = 0; i < 50; ++i)
    {
        buffer.push(message(i, 100, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
    }

    expect(buffer.bytes() <= 1000, "the byte bound is respected (" +
                                       std::to_string(buffer.bytes()) + " bytes)");
    expect(buffer.size() > 0, "and something is still retained");
    expect(buffer.evicted() == 50 - buffer.size(),
           "every message that went is counted (" + std::to_string(buffer.evicted()) + ")");
    expect(buffer.evictedBytes() > 0, "as are its bytes");

    // The NEWEST are kept. Keeping the oldest would mean a long session shows
    // you its first thirty seconds and nothing since, which is the opposite of
    // what a capture is for.
    std::uint64_t oldest_retained = 0;
    buffer.forEach(0, std::numeric_limits<std::uint64_t>::max(),
                   [&oldest_retained](const bag::QueuedMessage& seen)
                   {
                       if (oldest_retained == 0)
                       {
                           oldest_retained = seen.log_time_ns;
                       }
                       return true;
                   });
    expect(oldest_retained > kBase,
           "the OLDEST are evicted -- a capture keeps the newest, because that is what "
           "explains what is happening now");
}

// A single message larger than the whole cap must not wedge the buffer or
// evict forever. Reachable in practice: one H.264 keyframe is megabytes.
void testAnOversizedMessageIsSurvivable()
{
    scope::CaptureBuffer buffer(1000, 0.0);

    buffer.push(message(0, 100, kBase));
    buffer.push(message(1, 100'000, kBase + 1'000'000ull));

    expect(buffer.size() <= 1,
           "an oversized message evicts everything else rather than looping (" +
               std::to_string(buffer.size()) + " retained)");
    expect(countRetained(buffer) == buffer.size(), "and size() agrees with what is walkable");

    // Whatever survived, the buffer still accepts more.
    buffer.push(message(2, 100, kBase + 2'000'000ull));
    expect(buffer.size() >= 1, "and the buffer still accepts messages afterwards");
}

// -------------------------------------------------------------- time eviction

void testEvictsByTime()
{
    scope::CaptureBuffer buffer(0, 5.0);  // Five seconds.

    for (int i = 0; i < 20; ++i)
    {
        buffer.push(message(i, 16, kBase + static_cast<std::uint64_t>(i) * 1'000'000'000ull));
    }

    expect(buffer.retainedSpanSeconds() <= 5.0 + 1e-9,
           "the time bound is respected (" + std::to_string(buffer.retainedSpanSeconds()) +
               " s)");
    expect(buffer.size() == 6,
           "six one-second messages fit in a five-second window, inclusive at both ends (" +
               std::to_string(buffer.size()) + ")");
    expect(buffer.evicted() == 14, "and the rest are counted as evicted");
}

// THE case a buffer that checked only one bound would get wrong. Both are
// applied on every push, so whichever binds first wins -- and which one that is
// depends entirely on the workload.
void testWhicheverBoundBindsFirstWins()
{
    // Telemetry-shaped: tiny messages, slow. The TIME bound binds; the byte one
    // never would.
    {
        scope::CaptureBuffer buffer(1024 * 1024, 5.0);
        for (int i = 0; i < 20; ++i)
        {
            buffer.push(message(i, 16, kBase + static_cast<std::uint64_t>(i) * 1'000'000'000ull));
        }
        expect(buffer.evicted() > 0 && buffer.bytes() < 1024 * 1024,
               "small slow messages are bounded by TIME -- a byte-only cap would let a long "
               "session grow without limit");
    }

    // Video-shaped: large messages, fast. The BYTE bound binds; the time one
    // never would.
    {
        scope::CaptureBuffer buffer(100'000, 3600.0);
        for (int i = 0; i < 50; ++i)
        {
            buffer.push(message(i, 20'000, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
        }
        expect(buffer.bytes() <= 100'000 && buffer.retainedSpanSeconds() < 3600.0,
               "large fast messages are bounded by BYTES -- a time-only cap would be "
               "OOM-killed within minutes on a bus carrying video");
    }
}

// The unsigned subtraction in the time bound has to be guarded. A message whose
// log_time is smaller than the window would wrap the cutoff to near UINT64_MAX
// and evict the entire buffer on every push -- silently, because an empty
// capture looks exactly like a bus with no publishers.
void testSmallTimestampsDoNotWrapTheCutoff()
{
    scope::CaptureBuffer buffer(0, 5.0);

    for (int i = 0; i < 10; ++i)
    {
        buffer.push(message(i, 16, static_cast<std::uint64_t>(i) * 1'000'000ull));
    }

    expect(buffer.size() == 10,
           "timestamps smaller than the time window do not wrap the cutoff and wipe the "
           "buffer (" + std::to_string(buffer.size()) + " retained)");
}

// ------------------------------------------------------------------ bookkeeping

void testBoundsCanBeTightenedInPlace()
{
    // The bounds arrive with the workspace, after capture has already started.
    // Rebuilding the recorder to apply them would discard everything captured
    // before the file was opened.
    scope::CaptureBuffer buffer(0, 0.0);
    for (int i = 0; i < 50; ++i)
    {
        buffer.push(message(i, 100, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
    }
    expect(buffer.size() == 50, "everything is retained under no bounds");

    buffer.setBounds(1000, 0.0);
    expect(buffer.bytes() <= 1000, "tightening the bound evicts immediately");
    expect(buffer.evicted() > 0, "and counts what went");
}

void testClearKeepsTheLifetimeCounters()
{
    scope::CaptureBuffer buffer(1000, 0.0);
    for (int i = 0; i < 50; ++i)
    {
        buffer.push(message(i, 100, kBase + static_cast<std::uint64_t>(i) * 1'000'000ull));
    }

    const std::uint64_t evicted_before = buffer.evicted();
    expect(evicted_before > 0, "something was evicted before the clear");

    buffer.clear();
    expect(buffer.size() == 0, "clear empties the buffer");
    expect(buffer.bytes() == 0, "and its byte count");
    expect(buffer.evicted() == evicted_before,
           "but NOT the eviction counters -- zeroing them would make 'this session lost "
           "nothing' and 'this session was just restarted' look identical");
}

void testRevisionMovesOnEveryChange()
{
    scope::CaptureBuffer buffer(0, 0.0);
    const std::uint64_t initial = buffer.revision();

    buffer.push(message(0, 16, kBase));
    expect(buffer.revision() > initial, "a push moves the revision");

    const std::uint64_t after_push = buffer.revision();
    buffer.clear();
    expect(buffer.revision() > after_push,
           "so does a clear -- a reviewer polling this is how the scrubber learns its range "
           "changed under it");
}

// ---------------------------------------------------------------- concurrency

void testProducerAndConsumerUnderThreads()
{
    // The real producer is a zenoh RX thread and the real consumer is the GUI
    // thread, so this is threads rather than a simulation of them.
    scope::CaptureBuffer buffer(64 * 1024, 0.0);

    constexpr int kPushes = 20000;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> pushed{0};

    std::thread producer(
        [&]()
        {
            for (int i = 0; i < kPushes; ++i)
            {
                buffer.push(message(i, 64, kBase + static_cast<std::uint64_t>(i) * 1'000ull));
                ++pushed;
            }
            done = true;
        });

    // Consume until the producer is finished, rather than for a fixed number of
    // passes: 200 passes over an empty buffer complete before a thread has even
    // started, and the test would then pass without the two ever having
    // overlapped.
    bool always_ordered = true;
    std::uint64_t passes = 0;
    while (!done.load())
    {
        std::uint64_t last = 0;
        buffer.forEach(0, std::numeric_limits<std::uint64_t>::max(),
                       [&](const bag::QueuedMessage& seen)
                       {
                           if (seen.log_time_ns < last)
                           {
                               always_ordered = false;
                           }
                           last = seen.log_time_ns;
                           return true;
                       });
        (void)buffer.spanNanos();
        (void)buffer.retainedSpanSeconds();
        ++passes;

        // Hand the lock to the producer between passes. forEach holds the buffer
        // mutex for a whole traversal and this loop would otherwise re-acquire it
        // the instant it let go; pthread mutexes are not fair, so the producer
        // loses the race nearly every time and lands about one push per pass.
        // Optimised builds hide that -- a pass is quick, so 20000 pushes still
        // finish in under a second -- but at -O0 the passes stretch and the
        // starvation stretches with them, taking the test past its 120 s timeout.
        std::this_thread::yield();
    }

    producer.join();
    expect(passes > 0, "the consumer ran while the producer was pushing (" +
                           std::to_string(passes) + " passes)");

    expect(pushed == kPushes, "the producer pushed everything (" +
                                  std::to_string(pushed.load()) + ")");
    expect(always_ordered, "every snapshot the consumer saw was time-ordered");
    expect(buffer.bytes() <= 64 * 1024, "and the bound held throughout");
    expect(buffer.size() + buffer.evicted() == pushed,
           "every pushed message is either retained or counted as evicted -- nothing "
           "disappears unaccounted for (" +
               std::to_string(buffer.size()) + " + " + std::to_string(buffer.evicted()) +
               " vs " + std::to_string(pushed.load()) + ")");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testRetainsInOrder();
    testRangeQuery();

    testDensityAgainstBruteForce();
    testDensityBoundaryConvention();
    testDensityDegenerateArguments();
    testDensityOnAnEmptyBuffer();

    testEvictsByBytes();
    testAnOversizedMessageIsSurvivable();

    testEvictsByTime();
    testWhicheverBoundBindsFirstWins();
    testSmallTimestampsDoNotWrapTheCutoff();

    testBoundsCanBeTightenedInPlace();
    testClearKeepsTheLifetimeCounters();
    testRevisionMovesOnEveryChange();

    testProducerAndConsumerUnderThreads();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
