// SPDX-License-Identifier: GPL-3.0-or-later
//
// The raw-payload buffer: what it keeps, what it throws away, and whether it
// says so.
//
// The same two-bound argument CaptureBuffer makes, applied one layer up. A video
// panel holding a minute of CarPlay at 4 Mbit is ~30 MB; a buffer honouring only
// the bound its author expected to hit fails on the other workload, silently. So
// both are checked, and so is the case each one alone would get wrong.
//
// The seek path is checked separately from the streaming one because they are
// different code and fail differently: replaceHistory() is what keeps
// lowerBound()'s non-decreasing precondition true across a backwards scrub, and
// a buffer that kept its old contents would stay perfectly ordered and be
// completely wrong.
//
// No Qt, no zenoh, no ffmpeg -- a plain unit test.

#include "scope/raw_buffer.h"

#include <spdlog/spdlog.h>

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

scope::RawMessage message(double t, std::size_t payload_bytes, std::uint32_t flags = 0)
{
    scope::RawMessage out;
    out.t = t;
    out.payload.assign(payload_bytes, 0x5A);
    out.flags = flags;
    return out;
}

// One message's contribution to the byte count, so the tests can express their
// bounds in messages rather than in the struct's size.
std::size_t sizeOf(std::size_t payload_bytes)
{
    return scope::rawMessageBytes(message(0.0, payload_bytes));
}

// ---------------------------------------------------------------------------

void testTimeBoundEvictsOldest()
{
    // 10 s of retention, no byte bound. The workload the byte bound would never
    // bite on: small messages, a long session.
    scope::RawBuffer buffer(10.0, 0);

    for (int i = 0; i < 100; ++i)
    {
        buffer.push(message(static_cast<double>(i) * 0.5, 64));
    }
    buffer.drain(49.5);

    const scope::RawHistory& history = buffer.history();
    expect(!history.empty(), "time bound: something is retained");
    expect(history.oldest().t >= 39.5 - 1e-9,
           "time bound: nothing older than now - retention survives");
    expect(history.newest().t == 49.5, "time bound: the newest message is kept");
    expect(buffer.received() == 100, "time bound: received counts everything pushed");
    expect(buffer.dropped() == 0, "time bound: nothing was dropped at the producer");
}

void testByteBoundEvictsOldest()
{
    // The other workload: a byte bound that bites long before any time bound
    // would. This is the CarPlay case -- four 4 MB keyframes exceed most caps.
    const std::size_t cap = sizeOf(1000) * 10;
    scope::RawBuffer buffer(0.0, cap);

    for (int i = 0; i < 50; ++i)
    {
        buffer.push(message(static_cast<double>(i), 1000));
        buffer.drain(static_cast<double>(i));
    }

    const scope::RawHistory& history = buffer.history();
    expect(history.bytes() <= cap, "byte bound: the cap is respected");
    expect(history.size() == 10, "byte bound: exactly the cap's worth is retained");
    expect(history.newest().t == 49.0, "byte bound: the newest message is kept");
    expect(history.oldest().t == 40.0, "byte bound: the oldest were evicted first");
}

void testEachBoundCatchesWhatTheOtherMisses()
{
    // THE CASE A BUFFER HONOURING ONE BOUND GETS WRONG. Same buffer, two
    // workloads; each bound has to bite on the workload the other ignores.
    {
        // Telemetry-shaped: tiny messages over a long span. A byte-only buffer
        // would grow without limit here.
        scope::RawBuffer buffer(5.0, 100 * 1024 * 1024);
        for (int i = 0; i < 1000; ++i)
        {
            buffer.push(message(static_cast<double>(i) * 0.1, 32));
        }
        buffer.drain(99.9);
        expect(buffer.history().bytes() < 100 * 1024 * 1024,
               "mixed: the byte bound never bites on telemetry-shaped traffic");
        expect(buffer.history().oldest().t >= 94.9 - 1e-9,
               "mixed: the time bound is what limits telemetry-shaped traffic");
    }
    {
        // Video-shaped: a handful of huge messages inside one second. A
        // time-only buffer would hold every byte of it.
        const std::size_t cap = sizeOf(4 * 1024 * 1024) * 3;
        scope::RawBuffer buffer(600.0, cap);
        for (int i = 0; i < 10; ++i)
        {
            buffer.push(message(static_cast<double>(i) * 0.03, 4 * 1024 * 1024));
            buffer.drain(0.3);
        }
        expect(buffer.history().bytes() <= cap,
               "mixed: the byte bound is what limits video-shaped traffic");
        expect(buffer.history().size() == 3,
               "mixed: the time bound never bites inside a third of a second");
    }
}

void testProducerDropsNewestAndCountsIt()
{
    // The producer may not evict -- that would mean moving a position the
    // consumer owns -- so it drops the newest and says so. A drop here means the
    // GUI thread is wedged, and a silent one would make the panel look merely
    // stale rather than broken.
    const std::size_t cap = sizeOf(1000) * 3;
    scope::RawBuffer buffer(0.0, cap);

    for (int i = 0; i < 20; ++i)
    {
        buffer.push(message(static_cast<double>(i), 1000));
    }

    expect(buffer.dropped() > 0, "producer: overflow is counted rather than silent");

    buffer.drain(19.0);
    expect(buffer.history().oldest().t == 0.0,
           "producer: the OLDEST staged message survived, not the newest");
}

void testLowerBound()
{
    scope::RawBuffer buffer(0.0, 0);
    for (int i = 0; i < 10; ++i)
    {
        buffer.push(message(static_cast<double>(i), 16));
    }
    buffer.drain(9.0);

    const scope::RawHistory& history = buffer.history();
    expect(history.lowerBound(-1.0) == 0, "lowerBound: before everything is index 0");
    expect(history.lowerBound(0.0) == 0, "lowerBound: an exact hit is the element itself");
    expect(history.lowerBound(4.5) == 5, "lowerBound: between two elements rounds up");
    expect(history.lowerBound(9.0) == 9, "lowerBound: the last element");
    expect(history.lowerBound(100.0) == history.size(),
           "lowerBound: past everything is size()");
}

void testSeekReplacesRatherThanAppends()
{
    // THE BACKWARDS SEEK, and it is checked the way the plot's is: the assertion
    // is that the window MOVED BACK, not that it is still ordered. A buffer that
    // kept the position it came from stays perfectly ordered and is completely
    // wrong.
    scope::RawBuffer buffer(0.0, 0);

    std::vector<scope::RawMessage> forward;
    for (int i = 20; i < 30; ++i)
    {
        forward.push_back(message(static_cast<double>(i), 64));
    }
    buffer.replaceHistory(std::move(forward));

    expect(buffer.history().oldest().t == 20.0, "seek: the first window loaded");
    expect(buffer.history().newest().t == 29.0, "seek: the first window loaded fully");

    std::vector<scope::RawMessage> backward;
    for (int i = 5; i < 15; ++i)
    {
        backward.push_back(message(static_cast<double>(i), 64));
    }
    buffer.replaceHistory(std::move(backward));

    expect(buffer.history().size() == 10, "backwards seek: the old window is gone entirely");
    expect(buffer.history().oldest().t == 5.0, "backwards seek: t_first moved back");
    expect(buffer.history().newest().t == 14.0, "backwards seek: t_last moved BACK");

    // And the precondition the whole clear exists to protect.
    bool ordered = true;
    for (std::size_t i = 1; i < buffer.history().size(); ++i)
    {
        ordered = ordered && buffer.history()[i - 1].t <= buffer.history()[i].t;
    }
    expect(ordered, "backwards seek: times are still non-decreasing");
}

void testClearKeepsLifetimeCounters()
{
    // received() and dropped() are lifetime counters. Zeroing them on a scrub
    // would make "this stream has produced nothing" and "this stream was just
    // reloaded" look identical in the stats -- the one place a test can tell
    // them apart.
    scope::RawBuffer buffer(0.0, 0);
    for (int i = 0; i < 5; ++i)
    {
        buffer.push(message(static_cast<double>(i), 32));
    }
    buffer.drain(4.0);

    const std::uint64_t received = buffer.received();
    expect(received == 5, "clear: received counted the pushes");

    buffer.clear();
    expect(buffer.history().empty(), "clear: the history is empty");
    expect(buffer.received() == received, "clear: received is NOT reset");
}

void testFlagsSurviveTheRoundTrip()
{
    // The buffer must not interpret the classifier's answer, only carry it. A
    // buffer that masked or normalised these would break seeking without
    // breaking anything a screenshot would show.
    scope::RawBuffer buffer(0.0, 0);
    buffer.push(message(1.0, 16, scope::RawMessage::kPreamble));
    buffer.push(message(2.0, 16, scope::RawMessage::kSeekPoint));
    buffer.push(message(3.0, 16, 0));
    buffer.push(message(4.0, 16, scope::RawMessage::kFirstUserFlag | scope::RawMessage::kSeekPoint));
    buffer.drain(4.0);

    const scope::RawHistory& history = buffer.history();
    expect(history[0].flags == scope::RawMessage::kPreamble, "flags: preamble survived");
    expect(history[1].flags == scope::RawMessage::kSeekPoint, "flags: seek point survived");
    expect(history[2].flags == 0, "flags: an unflagged message stays unflagged");
    expect(history[3].flags ==
               (scope::RawMessage::kFirstUserFlag | scope::RawMessage::kSeekPoint),
           "flags: a consumer's own bits survive alongside a reserved one");
}

void testGenerationSeparatesReplacementFromTrimming()
{
    // A consumer decoding out of this cannot tell "the source moved me to a
    // different window" from "more arrived and the oldest was trimmed" by
    // looking at the contents: the oldest timestamp moves for BOTH. The video
    // panel used the oldest timestamp for exactly this and would therefore have
    // thrown its decoder away and re-decoded the whole retention window on every
    // tick, for ever, starting the moment retention first filled -- which is why
    // a short live run looked perfect.
    scope::RawBuffer buffer(5.0, 0);

    for (int i = 0; i < 10; ++i)
    {
        buffer.push(message(static_cast<double>(i), 64));
    }
    buffer.drain(9.0);

    const std::uint64_t after_growth = buffer.generation();
    const double oldest_before = buffer.history().oldest().t;

    // Another push, draining at a time that forces the front out.
    buffer.push(message(20.0, 64));
    buffer.drain(20.0);

    expect(buffer.history().oldest().t != oldest_before,
           "trimming really did move the oldest timestamp");
    expect(buffer.generation() == after_growth,
           "but the generation did NOT move -- growth and trimming are not a replacement");

    buffer.replaceHistory({message(100.0, 64)});
    expect(buffer.generation() != after_growth,
           "a seek's wholesale replace DOES move the generation");

    const std::uint64_t after_replace = buffer.generation();
    buffer.clear();
    expect(buffer.generation() != after_replace,
           "and so does a clear, which is a replacement with nothing");
}

void testConcurrentProducer()
{
    // The producer really is a zenoh RX thread, so the hand-off is checked under
    // real threads. The invariant is the accounting one: every pushed message is
    // either retained, evicted by a bound, or counted as dropped.
    scope::RawBuffer buffer(0.0, 0);

    constexpr int kPushes = 20000;
    std::thread producer(
        [&buffer]()
        {
            for (int i = 0; i < kPushes; ++i)
            {
                buffer.push(message(static_cast<double>(i) * 0.001, 64));
            }
        });

    for (int i = 0; i < 200; ++i)
    {
        buffer.drain(1000.0);
    }
    producer.join();
    buffer.drain(1000.0);

    expect(buffer.received() + buffer.dropped() == kPushes,
           "threads: every push is either received or counted as dropped");
    expect(buffer.history().size() == buffer.received(),
           "threads: with no bounds, everything received is retained");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    testTimeBoundEvictsOldest();
    testByteBoundEvictsOldest();
    testEachBoundCatchesWhatTheOtherMisses();
    testProducerDropsNewestAndCountsIt();
    testLowerBound();
    testSeekReplacesRatherThanAppends();
    testClearKeepsLifetimeCounters();
    testFlagsSurviveTheRoundTrip();
    testGenerationSeparatesReplacementFromTrimming();
    testConcurrentProducer();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
