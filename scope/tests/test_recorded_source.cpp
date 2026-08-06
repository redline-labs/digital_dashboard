// SPDX-License-Identifier: GPL-3.0-or-later
//
// RecordedSource: scrubbing a recording, and the ordering invariant that makes
// it safe.
//
// The one property everything else rests on is that SampleHistory's times stay
// NON-DECREASING. lowerBound() is a binary search over them and has no way to
// notice when they are not -- it returns a plausible wrong index instead of
// failing, and every autoscale, every decimation column and every cursor
// readout downstream is computed from it. A backwards seek is precisely where
// that invariant would be broken, which is why it is asserted directly here
// rather than inferred from a picture.
//
// Over a stub provider rather than a real bag. The point is the scrubbing
// logic, and synthetic messages make "seek to 5 s and you get exactly the
// samples in [5 - history, 5]" an exact assertion rather than an approximate
// one. bag_test_roundtrip already covers the file layer.
//
// No Qt widgets and no zenoh -- a plain unit test.

#include "scope/recorded_source.h"

#include "pub_sub/schema_registry.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <engine_rpm.capnp.h>

#include <spdlog/spdlog.h>

#include <chrono>
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

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;  // a plausible 2026 timestamp

// A real capnp EngineRpm payload, so the evaluator decodes it for real. A
// hand-rolled byte buffer would exercise the plumbing and none of the decoding,
// and decoding against the wrong schema is exactly the silent failure the
// schema check exists to prevent.
std::vector<std::uint8_t> rpmPayload(std::uint32_t rpm)
{
    capnp::MallocMessageBuilder builder;
    builder.initRoot<EngineRpm>().setRpm(rpm);

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(builder);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

// A recording held in memory: one message per 100 ms for `count` messages, with
// rpm counting up so a sample can be traced back to the message it came from.
class StubProvider : public scope::RecordedProvider
{
  public:
    StubProvider(std::size_t count, std::uint64_t step_ns) : step_ns_(step_ns)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            payloads_.push_back(rpmPayload(static_cast<std::uint32_t>(1000 + i)));
        }
    }

    void forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                 const std::function<void(const bag::BagMessage&)>& visit) override
    {
        ++passes;

        for (std::size_t i = 0; i < payloads_.size(); ++i)
        {
            const std::uint64_t log_time = kBase + static_cast<std::uint64_t>(i) * step_ns_;
            if (log_time < t0_ns || log_time > t1_ns)
            {
                continue;
            }

            bag::BagMessage message;
            message.key = "vehicle/engine/rpm";
            message.schema = "EngineRpm";
            message.payload = payloads_[i];
            message.log_time_ns = log_time;
            message.publish_time_ns = log_time;
            visit(message);
        }
    }

    std::vector<scope::TopicInfo> topics() const override
    {
        return {scope::TopicInfo{"vehicle/engine/rpm", "EngineRpm", true},
                scope::TopicInfo{"vehicle/never_published", "VehicleSpeed", false}};
    }

    std::pair<std::uint64_t, std::uint64_t> spanNanos() const override
    {
        if (payloads_.empty())
        {
            return {0, 0};
        }
        return {kBase, kBase + static_cast<std::uint64_t>(payloads_.size() - 1) * step_ns_};
    }

    // Counted so the "decode once per signal, not once per scrub tick" claim is
    // an assertion rather than a comment.
    std::atomic<int> passes{0};

  private:
    std::uint64_t step_ns_;
    std::vector<std::vector<std::uint8_t>> payloads_;
};

scope::SignalKey rpmKey()
{
    scope::SignalKey key;
    key.zenoh_key = "vehicle/engine/rpm";
    key.schema_type = pub_sub::schema_type_t::EngineRpm;
    key.value_expression = "rpm";
    return key;
}

// Binding starts a decode on a background thread, so a test that read the
// buffer immediately would be racing it. Waiting on decodesPending() rather
// than sleeping a fixed interval keeps this deterministic on a loaded machine.
bool waitForDecode(scope::RecordedSource& source)
{
    for (int i = 0; i < 500; ++i)
    {
        if (source.decodesPending() == 0)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Is every retained sample at or after the one before it? THE invariant.
bool isOrdered(const scope::SampleHistory& history)
{
    for (std::size_t i = 1; i < history.size(); ++i)
    {
        if (history[i].t < history[i - 1].t)
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------- caps

void testCapsDescribeTheRecording()
{
    // 100 messages at 100 ms is 9.9 s from first to last.
    auto provider = std::make_unique<StubProvider>(100, 100'000'000ull);
    scope::RecordedSource source(std::move(provider));

    const scope::SourceCaps caps = source.caps();
    expect(!caps.live, "a recording is not live");
    expect(caps.seekable, "a recording is seekable -- which is the whole point");
    expect(caps.t_begin == 0.0, "the epoch starts at zero, matching the live source's shape");
    expect(std::abs(caps.t_end - 9.9) < 1e-6,
           "t_end is the recording's duration in seconds (" + std::to_string(caps.t_end) + ")");

    expect(std::abs(source.now() - 9.9) < 1e-6,
           "a freshly opened recording sits at the END -- what happened is usually at the end, "
           "and starting at zero shows an empty window that reads as an empty bag");
}

void testTopicsComeFromTheIndex()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(10, 100'000'000ull));

    const std::vector<scope::TopicInfo> topics = source.topics();
    expect(topics.size() == 2, "both topics are listed");

    bool found_silent = false;
    for (const scope::TopicInfo& topic : topics)
    {
        if (topic.key == "vehicle/never_published")
        {
            found_silent = !topic.reachable;
        }
    }
    expect(found_silent,
           "a topic that was advertised and never published is listed but not reachable -- "
           "only liveliness could have said so, and it is the difference between 'no data' "
           "and 'that node was not running'");
}

// ------------------------------------------------------------------- decoding

void testBindDecodesOnceForTheWholeRecording()
{
    auto owned = std::make_unique<StubProvider>(100, 100'000'000ull);
    StubProvider* const provider = owned.get();
    scope::RecordedSource source(std::move(owned));

    auto buffer = std::make_shared<scope::SignalBuffer>(30.0, 100000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(handle != scope::kInvalidSignal, "a valid signal binds");
    expect(waitForDecode(source), "the decode finishes");

    expect(provider->passes == 1, "binding costs exactly ONE pass over the recording (" +
                                      std::to_string(provider->passes.load()) + ")");

    // THE claim the whole design rests on. BagReader::forEach opens an mcap
    // reader per part per call and, on a torn part, scans the entire data
    // section; driven from a slider it would do that thirty times a second.
    for (int i = 0; i < 20; ++i)
    {
        source.seek(static_cast<double>(i) * 0.4);
    }
    expect(provider->passes == 1,
           "and twenty seeks cost NONE -- scrubbing is a slice out of the decoded vector, not "
           "a re-read of the file (" + std::to_string(provider->passes.load()) + " passes)");

    source.release(handle);
}

void testABadExpressionIsRefusedImmediately()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(10, 100'000'000ull));

    scope::SignalKey key = rpmKey();
    key.value_expression = "no_such_field * 2";

    auto buffer = std::make_shared<scope::SignalBuffer>(30.0, 1000, 4096);
    expect(source.bind(key, buffer) == scope::kInvalidSignal,
           "an expression naming a field the schema does not have is a definite no, not a "
           "handle whose decode quietly produces nothing");

    expect(source.bind(rpmKey(), nullptr) == scope::kInvalidSignal, "a null buffer is refused");
}

// -------------------------------------------------------------------- seeking

void testSeekFillsTheWindow()
{
    // 100 messages at 100 ms, retention 2 s -- so a window holds ~21 samples
    // and the retention limit genuinely bites.
    scope::RecordedSource source(std::make_unique<StubProvider>(100, 100'000'000ull));

    auto buffer = std::make_shared<scope::SignalBuffer>(2.0, 100000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(waitForDecode(source), "the decode finishes");

    source.seek(5.0);
    expect(std::abs(source.now() - 5.0) < 1e-9, "now() follows the seek");

    const scope::SampleHistory& history = buffer->history();
    expect(!history.empty(), "the window is filled");
    if (!history.empty())
    {
        expect(history.newest().t <= 5.0 + 1e-9,
               "nothing after the playback head is shown -- a plot must not draw the future");
        expect(history.oldest().t >= 3.0 - 1e-9,
               "and nothing older than the retention window (" +
                   std::to_string(history.oldest().t) + ")");

        // rpm counts up from 1000 at 100 ms, so t = 5.0 is message 50.
        expect(history.newest().v == 1050.0,
               "the value at the head is the recorded one, decoded through the real schema (" +
                   std::to_string(history.newest().v) + ")");
    }
    expect(isOrdered(history), "the window is time-ordered");

    source.release(handle);
}

// THE REGRESSION THIS FILE EXISTS FOR.
//
// SampleHistory::lowerBound() is a binary search that assumes non-decreasing
// time. Scrubbing backwards is the one operation that can violate it, and the
// violation is silent: the search returns a plausible wrong index, and the
// autoscale, the decimation and the cursor readout all quietly compute from the
// wrong samples. Nothing throws, nothing logs, and the plot looks like data.
//
// The two halves are checked separately here because they fail differently, and
// both were confirmed by mutation:
//
//   - Drop the clear() from SignalBuffer::replaceHistory() and the buffer holds
//     the old window followed by the new one. Times go backwards mid-buffer:
//     the isOrdered() assertion below is the one that fires.
//   - Drop the `end < filled_to` rebuild from RecordedSource::refill() and the
//     buffer is never rebuilt at all, so it keeps showing the position it came
//     from. Still ordered, still wrong -- which is why the newest-sample
//     assertions are here and not left to the ordering check.
void testSeekingBackwardsKeepsTheBufferOrdered()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(100, 100'000'000ull));

    auto buffer = std::make_shared<scope::SignalBuffer>(2.0, 100000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(waitForDecode(source), "the decode finishes");

    source.seek(8.0);
    const std::size_t forward_size = buffer->history().size();
    expect(forward_size > 0, "the forward window has samples");

    source.seek(1.0);  // BACKWARDS.

    const scope::SampleHistory& history = buffer->history();
    expect(isOrdered(history),
           "the buffer is still time-ordered after seeking backwards -- if it is not, "
           "lowerBound() returns a plausible wrong index and every reading taken from it is "
           "silently wrong");

    if (!history.empty())
    {
        expect(history.newest().t <= 1.0 + 1e-9,
               "and holds NOTHING from the position it came from (newest is " +
                   std::to_string(history.newest().t) + ")");
        expect(history.newest().v == 1010.0,
               "the value at the new head is the one recorded there (" +
                   std::to_string(history.newest().v) + ")");
    }

    // lowerBound is what everything downstream uses; check it answers correctly
    // rather than only that the order looks right.
    const std::size_t at = history.lowerBound(0.5);
    expect(at < history.size() && history[at].t >= 0.5 - 1e-9,
           "lowerBound finds the first sample at or after a time in the new window");
    expect(at == 0 || history[at - 1].t < 0.5, "and nothing before it is at or after that time");

    source.release(handle);
}

void testSeekingIsClampedToTheRecording()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(50, 100'000'000ull));

    source.seek(-100.0);
    expect(source.now() == 0.0, "seeking before the start clamps to the start");

    source.seek(1e9);
    expect(std::abs(source.now() - source.caps().t_end) < 1e-9,
           "seeking past the end clamps to the end, rather than showing an empty window that "
           "looks like a publisher that stopped");
}

// --------------------------------------------------------------------- playing

void testPlaybackAdvancesAndStops()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(30, 100'000'000ull));

    auto buffer = std::make_shared<scope::SignalBuffer>(5.0, 100000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(waitForDecode(source), "the decode finishes");

    source.seek(0.0);
    source.setRate(1.0);
    source.setPlaying(true);

    // Ticks are driven by TimeBase's timer in the app; here they are driven by
    // hand, with real elapsed time between them.
    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        source.tick();
    }

    expect(source.now() > 0.0, "playing advances the position (" +
                                   std::to_string(source.now()) + ")");
    expect(isOrdered(buffer->history()), "and the buffer stays ordered while playing");

    // Running off the end stops rather than continuing past it.
    source.seek(source.caps().t_end - 0.05);
    source.setPlaying(true);
    for (int i = 0; i < 20; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        source.tick();
    }
    expect(std::abs(source.now() - source.caps().t_end) < 1e-9,
           "playback stops AT the end rather than running past it");

    source.release(handle);
}

void testWallClockIsTheRecordingsOwn()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(100, 100'000'000ull));

    expect(source.wallClockNanosAt(0.0) == kBase,
           "position zero is the first message's log time -- the absolute time a bag genuinely "
           "has and the live source does not");
    expect(source.wallClockNanosAt(1.0) == kBase + 1'000'000'000ull,
           "and a second in is a second later");
}

// ---------------------------------------------------------------- degenerate

void testAnEmptyRecordingIsSurvivable()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(0, 100'000'000ull));

    expect(source.caps().t_end == 0.0, "an empty recording has zero duration");
    expect(source.now() == 0.0, "and sits at zero");

    auto buffer = std::make_shared<scope::SignalBuffer>(5.0, 1000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(handle != scope::kInvalidSignal,
           "binding still succeeds -- an empty recording is not a broken one");
    expect(waitForDecode(source), "the decode finishes");

    source.seek(0.0);
    expect(buffer->history().empty(), "and produces no samples rather than inventing any");

    source.release(handle);
}

void testReleaseIsIdempotent()
{
    scope::RecordedSource source(std::make_unique<StubProvider>(10, 100'000'000ull));

    auto buffer = std::make_shared<scope::SignalBuffer>(5.0, 1000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(waitForDecode(source), "the decode finishes");

    source.release(handle);
    source.release(handle);  // Again.
    source.release(12345);   // Never issued.

    // Nothing to assert but "did not crash", which is the point: a panel
    // releases in its destructor and the window may already have swapped the
    // source underneath it.
    expect(true, "releasing twice, or releasing a handle that was never issued, is harmless");
}

// A bag whose messages carry a different schema than the binding expects must
// not be decoded. capnp reads whatever bytes it is handed against whatever
// schema it is given -- field offsets just land on different bytes -- so the
// result is a plausible number, not an error.
class WrongSchemaProvider : public scope::RecordedProvider
{
  public:
    WrongSchemaProvider() : payload_(rpmPayload(4200)) {}

    void forEach(std::uint64_t, std::uint64_t,
                 const std::function<void(const bag::BagMessage&)>& visit) override
    {
        for (int i = 0; i < 10; ++i)
        {
            bag::BagMessage message;
            message.key = "vehicle/engine/rpm";
            message.schema = "VehicleSpeed";  // NOT what the binding asks for.
            message.payload = payload_;
            message.log_time_ns = kBase + static_cast<std::uint64_t>(i) * 100'000'000ull;
            message.publish_time_ns = message.log_time_ns;
            visit(message);
        }
    }

    std::vector<scope::TopicInfo> topics() const override { return {}; }
    std::pair<std::uint64_t, std::uint64_t> spanNanos() const override
    {
        return {kBase, kBase + 900'000'000ull};
    }

  private:
    std::vector<std::uint8_t> payload_;
};

void testAMismatchedSchemaIsNotDecoded()
{
    scope::RecordedSource source(std::make_unique<WrongSchemaProvider>());

    auto buffer = std::make_shared<scope::SignalBuffer>(5.0, 1000, 4096);
    const scope::SignalHandle handle = source.bind(rpmKey(), buffer);
    expect(waitForDecode(source), "the decode finishes");

    source.seek(source.caps().t_end);
    expect(buffer->history().empty(),
           "messages recorded under a different schema are skipped, not decoded -- capnp would "
           "read them happily and produce a plausible wrong number rather than an error");

    source.release(handle);
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testCapsDescribeTheRecording();
    testTopicsComeFromTheIndex();

    testBindDecodesOnceForTheWholeRecording();
    testABadExpressionIsRefusedImmediately();

    testSeekFillsTheWindow();
    testSeekingBackwardsKeepsTheBufferOrdered();
    testSeekingIsClampedToTheRecording();

    testPlaybackAdvancesAndStops();
    testWallClockIsTheRecordingsOwn();

    testAnEmptyRecordingIsSurvivable();
    testReleaseIsIdempotent();
    testAMismatchedSchemaIsNotDecoded();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
