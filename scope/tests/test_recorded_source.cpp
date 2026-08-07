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
#include <cmath>
#include <functional>
#include <span>
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

// ------------------------------------------------------- the raw / video path
//
// A recording shaped like an encoded video stream: repeating groups of
// [parameter sets][keyframe][deltas...]. The payloads are not real H.264 -- the
// decoding is VideoDecoder's problem and is covered by scope_test_video_decoder
// -- but the STRUCTURE is exactly the phone's, because that structure is what
// the windowing has to get right.
//
// Byte 0 carries the flags the classifier will report and byte 1 the message's
// ordinal, so a loaded window can be checked message by message.
constexpr std::size_t kGopLength = 10;  // one config + one keyframe + eight deltas

class GopProvider : public scope::RecordedProvider
{
  public:
    GopProvider(std::size_t gops, std::uint64_t step_ns) : step_ns_(step_ns)
    {
        for (std::size_t g = 0; g < gops; ++g)
        {
            for (std::size_t i = 0; i < kGopLength; ++i)
            {
                std::uint8_t flags = 0;
                if (i == 0)
                {
                    flags = scope::RawMessage::kPreamble;   // parameter sets
                }
                else if (i == 1)
                {
                    flags = scope::RawMessage::kSeekPoint;  // the keyframe
                }
                payloads_.push_back({flags, static_cast<std::uint8_t>(payloads_.size())});
            }
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
            message.key = "nodes/carplay/video";
            message.schema = "CarPlayVideo";
            message.payload = payloads_[i];
            message.log_time_ns = log_time;
            message.publish_time_ns = log_time;
            visit(message);
        }
    }

    std::vector<scope::TopicInfo> topics() const override
    {
        return {scope::TopicInfo{"nodes/carplay/video", "CarPlayVideo", true}};
    }

    std::pair<std::uint64_t, std::uint64_t> spanNanos() const override
    {
        if (payloads_.empty())
        {
            return {0, 0};
        }
        return {kBase, kBase + static_cast<std::uint64_t>(payloads_.size() - 1) * step_ns_};
    }

    // How many whole-recording passes were made. The index pass is one; a window
    // load is another, but a NARROW one -- which is the distinction this counter
    // exists to let a test assert.
    std::atomic<int> passes{0};

  private:
    std::uint64_t step_ns_;
    std::vector<std::vector<std::uint8_t>> payloads_;
};

// What the panel's classifier does, minus the capnp: read the flags the producer
// put in the payload. The SOURCE never interprets these -- that is the whole
// point of the classifier seam -- so the test supplies them the same way the
// panel does.
std::uint32_t classifyGop(std::span<const std::uint8_t> payload)
{
    return payload.empty() ? 0u : static_cast<std::uint32_t>(payload[0]);
}

// Run render ticks until `done` is true or the budget runs out.
//
// A window load runs on the worker and is published by a later tick(), so a test
// that read the buffer straight after seek() would be racing it. Ticking rather
// than sleeping keeps this deterministic: the publish only ever happens inside
// tick(), so a fixed number of them is a real bound rather than a hope.
bool pump(scope::RecordedSource& source, const std::function<bool()>& done)
{
    for (int i = 0; i < 500; ++i)
    {
        source.tick();
        if (done())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
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

// ------------------------------------------------------------- the raw path

void testRawBindingIndexesWithoutHoldingPayloads()
{
    // Four GOPs at 100 ms. The index pass reads the whole recording; the
    // payloads are NOT held, which is the entire reason the raw path exists --
    // decoding a video topic the way bind() decodes a signal would be ~900 MB
    // for half an hour.
    auto provider = std::make_unique<GopProvider>(4, 100'000'000ull);
    GopProvider* raw_provider = provider.get();
    scope::RecordedSource source(std::move(provider));

    auto buffer = std::make_shared<scope::RawBuffer>(0.0, 0);
    const scope::SignalHandle handle = source.bindRaw(
        "nodes/carplay/video", pub_sub::schema_type_t::CarPlayVideo, buffer, classifyGop);

    expect(handle != scope::kInvalidSignal, "raw: the binding was accepted");
    expect(waitForDecode(source), "raw: the index pass finished");
    expect(raw_provider->passes.load() >= 1, "raw: the index cost one pass over the recording");

    // Nothing is in the buffer yet: the index holds no payloads at all.
    expect(buffer->history().empty(),
           "raw: indexing loaded NO payloads -- that is the whole point");
}

void testSeekLoadsExactlyOneGop()
{
    auto provider = std::make_unique<GopProvider>(4, 100'000'000ull);
    GopProvider* raw_provider = provider.get();
    scope::RecordedSource source(std::move(provider));

    auto buffer = std::make_shared<scope::RawBuffer>(0.0, 0);
    source.bindRaw("nodes/carplay/video", pub_sub::schema_type_t::CarPlayVideo, buffer,
                   classifyGop);
    expect(waitForDecode(source), "gop: the index pass finished");

    const int passes_after_index = raw_provider->passes.load();

    // Into the middle of the third GOP. Messages 20..29 are that GOP; message 20
    // is its parameter sets and 21 its keyframe, at 2.0 s and 2.1 s.
    source.seek(2.5);
    expect(pump(source, [&]() { return !buffer->history().empty(); }),
           "gop: a window arrived after the seek");

    const scope::RawHistory& history = buffer->history();
    expect(history.size() == kGopLength,
           "gop: exactly ONE group was loaded, not the whole recording");
    expect(std::abs(history.oldest().t - 2.0) < 1e-6,
           "gop: the window STARTS at the parameter sets, not at the keyframe -- "
           "a window starting one message later decodes to nothing at all");
    expect((history.oldest().flags & scope::RawMessage::kPreamble) != 0,
           "gop: and that first message really is the preamble");
    expect(std::abs(history.newest().t - 2.9) < 1e-6,
           "gop: and STOPS before the next group's preamble");

    // The read was narrow. The index pass is a whole-recording scan and is paid
    // once; a window load must not be another one.
    expect(raw_provider->passes.load() == passes_after_index + 1,
           "gop: the window cost exactly one further read");

    // Scrubbing WITHIN the loaded group reads nothing at all.
    const int passes_after_window = raw_provider->passes.load();
    source.seek(2.7);
    pump(source, [&]() { return false; });
    expect(raw_provider->passes.load() == passes_after_window,
           "gop: scrubbing inside the loaded group reads the file ZERO more times");
}

void testRawSeekingBackwardsMovesTheWindow()
{
    // THE BACKWARDS CASE, asserted the way the numeric one is: the check is that
    // t_last MOVED BACK, not that the buffer is still ordered. A buffer that
    // kept the group it came from stays perfectly ordered and is completely
    // wrong -- and on screen it is a picture, so nothing looks broken.
    auto provider = std::make_unique<GopProvider>(5, 100'000'000ull);
    scope::RecordedSource source(std::move(provider));

    auto buffer = std::make_shared<scope::RawBuffer>(0.0, 0);
    source.bindRaw("nodes/carplay/video", pub_sub::schema_type_t::CarPlayVideo, buffer,
                   classifyGop);
    expect(waitForDecode(source), "backwards: the index pass finished");

    // 4.5, not 4.0. At exactly 4.0 the newest message is the last group's
    // PREAMBLE and its keyframe is still 100 ms in the future, so the correct
    // answer there is the group before -- which is right, and makes a poor
    // fixture for "seek somewhere late".
    source.seek(4.5);
    expect(pump(source, [&]() { return !buffer->history().empty(); }),
           "backwards: the late window arrived");

    const double late_first = buffer->history().oldest().t;
    const double late_last = buffer->history().newest().t;
    expect(std::abs(late_first - 4.0) < 1e-6,
           "backwards: the late window is the group containing 4.5 s");

    source.seek(0.5);
    expect(pump(source,
                [&]() {
                    return !buffer->history().empty() &&
                           buffer->history().newest().t < late_last;
                }),
           "backwards: an earlier window arrived");

    const scope::RawHistory& history = buffer->history();
    expect(history.oldest().t < late_first, "backwards: t_first moved BACK");
    expect(history.newest().t < late_last, "backwards: t_last moved BACK");
    expect(history.size() == kGopLength, "backwards: it is still exactly one group");

    // And the precondition the wholesale replace exists to protect.
    bool ordered = true;
    for (std::size_t i = 1; i < history.size(); ++i)
    {
        ordered = ordered && history[i - 1].t <= history[i].t;
    }
    expect(ordered, "backwards: the window is internally ordered");
    expect(std::abs(history.oldest().t - 0.0) < 1e-6,
           "backwards: it is the FIRST group, which is the one containing 0.5 s");
}

void testRawSkipsAMismatchedSchema()
{
    // A message recorded under a different schema is skipped rather than handed
    // over. Feeding a decoder another topic's bytes produces a plausible mess,
    // not an error.
    auto provider = std::make_unique<GopProvider>(3, 100'000'000ull);
    scope::RecordedSource source(std::move(provider));

    auto buffer = std::make_shared<scope::RawBuffer>(0.0, 0);
    source.bindRaw("nodes/carplay/video", pub_sub::schema_type_t::EngineRpm, buffer,
                   classifyGop);
    expect(waitForDecode(source), "schema: the index pass finished");

    source.seek(1.5);
    pump(source, [&]() { return false; });

    expect(buffer->history().empty(),
           "schema: a binding expecting another schema gets nothing, not garbage");
}

void testReleasingRawClearsThePendingCount()
{
    // decodesPending() is what a test and `scope.source` wait on to know the
    // picture is finished. Releasing a binding before its pass started used to
    // erase the job and leave the count, so the flag never came back to zero and
    // anything waiting on it hung -- and rebinding every panel across a source
    // swap is exactly the case that hits it.
    auto provider = std::make_unique<GopProvider>(3, 100'000'000ull);
    scope::RecordedSource source(std::move(provider));

    auto first = std::make_shared<scope::RawBuffer>(0.0, 0);
    auto second = std::make_shared<scope::RawBuffer>(0.0, 0);

    const scope::SignalHandle a = source.bindRaw(
        "nodes/carplay/video", pub_sub::schema_type_t::CarPlayVideo, first, classifyGop);
    const scope::SignalHandle b = source.bindRaw(
        "nodes/carplay/video", pub_sub::schema_type_t::CarPlayVideo, second, classifyGop);

    source.releaseRaw(a);
    source.releaseRaw(b);

    expect(waitForDecode(source),
           "release: decodesPending comes back to zero after releasing both bindings");
}

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

    testRawBindingIndexesWithoutHoldingPayloads();
    testSeekLoadsExactlyOneGop();
    testRawSeekingBackwardsMovesTheWindow();
    testRawSkipsAMismatchedSchema();
    testReleasingRawClearsThePendingCount();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

