// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writing a bag and reading it back: order, contents, splitting and seeking.
//
// These are the four properties the whole format choice rests on, and each
// fails quietly if it is wrong:
//
//   - CONTENTS. A payload that comes back altered is not a crash; it is a capnp
//     message that decodes into different numbers. The bytes are compared
//     exactly for that reason.
//   - ORDER. A bag's log_time ordering is what a player's timing and a scope's
//     scrubbing are built on. Out-of-order messages replay as jitter that looks
//     like it came from the vehicle.
//   - SPLITTING. Rolling exists so a crash costs one part rather than the
//     recording. A reader that dropped messages at a part boundary would lose
//     data at exactly the seam nobody looks at.
//   - SEEKING. `bag play --start-offset` into a multi-gigabyte recording has to
//     be a seek. A reader that quietly scanned instead would still return the
//     right messages -- just minutes later.
//
// No zenoh anywhere: this is file I/O against a temporary directory.

#include "bag/reader.h"
#include "bag/validate.h"
#include "bag/writer.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>
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

// A temporary directory that cleans itself up, so a failing test does not leave
// gigabytes behind and a passing one does not depend on what a previous run
// left.
class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("redline_bag_test_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string str() const { return path_.string(); }

  private:
    std::filesystem::path path_;
};

// A deterministic payload for message `i`, so a mix-up between messages is
// visible rather than merely a size mismatch.
std::vector<std::uint8_t> payloadFor(int index, std::size_t size)
{
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        payload[i] = static_cast<std::uint8_t>((index * 31 + static_cast<int>(i) * 7) & 0xFF);
    }
    return payload;
}

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;  // a plausible 2026 timestamp

// ---------------------------------------------------------------- round trip

void testRoundTrip()
{
    const TempDir dir("roundtrip");
    constexpr int kCount = 200;

    {
        bag::WriterOptions options;
        options.name = "trip";
        bag::BagWriter writer(dir.str(), options);
        expect(writer.isValid(), "the writer opens");

        for (int i = 0; i < kCount; ++i)
        {
            const std::vector<std::uint8_t> payload = payloadFor(i, 64);
            const bool ok = writer.write(i % 2 == 0 ? "vehicle/engine/rpm" : "vehicle/speed_mps",
                                         i % 2 == 0 ? "EngineRpm" : "VehicleSpeed", payload,
                                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull,
                                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull -
                                             500'000ull,
                                         "abc123");
            if (!ok)
            {
                expect(false, "write " + std::to_string(i) + " succeeded");
                break;
            }
        }

        expect(writer.close(), "the writer closes and writes metadata.yaml");
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the bag reads back");
    if (!reader.isValid())
    {
        return;
    }

    expect(reader.metadata().message_count == kCount,
           "metadata counts every message (" + std::to_string(reader.metadata().message_count) +
               ")");
    expect(reader.metadata().unstamped_messages == 0,
           "nothing was unstamped -- every message had a publish time");
    expect(reader.problems().empty(), "a clean recording reports no problems");

    int seen = 0;
    bool contents_match = true;
    bool ordered = true;
    bool schemas_match = true;
    bool publish_times_match = true;
    std::uint64_t previous = 0;

    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            const std::vector<std::uint8_t> expected = payloadFor(seen, 64);
            if (message.payload.size() != expected.size() ||
                !std::equal(expected.begin(), expected.end(), message.payload.begin()))
            {
                contents_match = false;
            }

            if (message.log_time_ns < previous)
            {
                ordered = false;
            }
            previous = message.log_time_ns;

            const std::string_view want_key =
                seen % 2 == 0 ? "vehicle/engine/rpm" : "vehicle/speed_mps";
            const std::string_view want_schema = seen % 2 == 0 ? "EngineRpm" : "VehicleSpeed";
            if (message.key != want_key || message.schema != want_schema)
            {
                schemas_match = false;
            }

            if (message.publish_time_ns != message.log_time_ns - 500'000ull)
            {
                publish_times_match = false;
            }

            ++seen;
            return true;
        });

    expect(seen == kCount, "every message comes back (" + std::to_string(seen) + ")");
    expect(contents_match, "every payload is byte-identical to what was written");
    expect(ordered, "messages come back in log_time order");
    expect(schemas_match, "each message keeps its key and its REGISTRY schema name");
    expect(publish_times_match, "publish_time survives distinct from log_time");
}

// An unstamped message borrows log_time -- and the bag says how often, because
// otherwise a file full of synthesised publish times looks exactly like one with
// real ones.
void testUnstampedAreCounted()
{
    const TempDir dir("unstamped");

    {
        bag::WriterOptions options;
        options.name = "unstamped";
        bag::BagWriter writer(dir.str(), options);

        for (int i = 0; i < 10; ++i)
        {
            const std::vector<std::uint8_t> payload = payloadFor(i, 32);
            // Half stamped, half not.
            const std::optional<std::uint64_t> publish =
                i % 2 == 0 ? std::optional<std::uint64_t>(kBase + i) : std::nullopt;
            writer.write("t", "EngineRpm", payload, kBase + static_cast<std::uint64_t>(i),
                         publish, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the bag reads back");
    expect(reader.metadata().unstamped_messages == 5,
           "the unstamped messages are counted (" +
               std::to_string(reader.metadata().unstamped_messages) + " of 10)");

    bool fallback_correct = true;
    int index = 0;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            if (index % 2 != 0 && message.publish_time_ns != message.log_time_ns)
            {
                fallback_correct = false;
            }
            ++index;
            return true;
        });

    expect(fallback_correct, "an unstamped message's publish_time equals its log_time");
}

// ------------------------------------------------------------------ splitting

void testSplitting()
{
    const TempDir dir("split");
    constexpr int kCount = 400;

    {
        bag::WriterOptions options;
        options.name = "split";
        options.compression = "none";  // so the size limit bites predictably
        options.chunk_bytes = 4 * 1024;
        // Small enough that 400 x 1 KiB messages must roll several times.
        options.max_part_bytes = 64 * 1024;

        bag::BagWriter writer(dir.str(), options);
        expect(writer.isValid(), "the writer opens");

        for (int i = 0; i < kCount; ++i)
        {
            writer.write("vehicle/engine/rpm", "EngineRpm", payloadFor(i, 1024),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the split bag reads back");
    if (!reader.isValid())
    {
        return;
    }

    expect(reader.metadata().parts.size() > 1,
           "the recording rolled into several parts (" +
               std::to_string(reader.metadata().parts.size()) + ")");

    // Parts must be contiguous and non-overlapping in time, which is what lets
    // the reader visit them in order and skip whole parts when seeking.
    bool contiguous = true;
    std::uint64_t previous_end = 0;
    std::uint64_t counted = 0;
    for (const bag::bag_part_t& part : reader.metadata().parts)
    {
        if (part.message_count == 0)
        {
            continue;
        }
        if (part.t_begin_ns < previous_end)
        {
            contiguous = false;
        }
        previous_end = part.t_end_ns;
        counted += part.message_count;
        expect(part.complete, "part '" + part.path + "' has a summary");
    }
    expect(contiguous, "parts do not overlap in time");
    expect(counted == kCount, "the parts' counts add up to the total");

    // THE assertion: the split is invisible.
    int seen = 0;
    bool ordered = true;
    bool contents_match = true;
    std::uint64_t last = 0;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            if (message.log_time_ns < last)
            {
                ordered = false;
            }
            last = message.log_time_ns;

            const std::vector<std::uint8_t> expected = payloadFor(seen, 1024);
            if (message.payload.size() != expected.size() ||
                !std::equal(expected.begin(), expected.end(), message.payload.begin()))
            {
                contents_match = false;
            }
            ++seen;
            return true;
        });

    expect(seen == kCount,
           "every message comes back across the part boundaries (" + std::to_string(seen) + ")");
    expect(ordered, "the stream stays ordered across parts");
    expect(contents_match, "no message is altered or duplicated at a seam");
}

// THE REGRESSION. A rolled part must be a self-consistent MCAP file.
//
// mcap::McapWriter deliberately RETAINS its schemas and channels across
// close()/open() -- its own documentation offers that as an efficiency for
// writing several files. Reusing one writer across a roll therefore produced a
// second part whose summary listed the previous part's schema and channel ids
// alongside freshly-assigned ones, referencing records its data section never
// contained.
//
// Nothing in this tree noticed. BagReader reads channels through the message
// views and found every message; `bag info` reported correct counts; the
// round-trip and splitting cases above passed. Only Foxglove's own validator
// disagreed:
//
//     $ mcap doctor final_0001.mcap
//     Error: Schema with id 1 in summary section does not exist in data section
//     Error: Encountered Channel (1) with unknown Schema (1)
//
// That is the shape of bug worth a test: correct through our own code, and
// malformed for every other reader of a format we chose *because* other readers
// exist. The fix is a fresh writer per part; this asserts the property directly
// rather than relying on an external tool being installed.
//
// Mutation-check: make Impl::openPart() reuse one writer instead of
// constructing a new one, and this fails while every other case still passes.
void testRolledPartsAreSelfConsistent()
{
    const TempDir dir("consistent");

    {
        bag::WriterOptions options;
        options.name = "consistent";
        options.compression = "none";
        options.chunk_bytes = 2 * 1024;
        options.max_part_bytes = 16 * 1024;

        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < 400; ++i)
        {
            // Three topics, so a part that duplicated its channel table would
            // report six.
            const char* keys[] = {"a/one", "a/two", "a/three"};
            const char* schemas[] = {"EngineRpm", "VehicleSpeed", "EngineTemperature"};
            writer.write(keys[i % 3], schemas[i % 3], payloadFor(i, 512),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.metadata().parts.size() > 1, "the recording rolled");

    bool all_consistent = true;
    for (const bag::bag_part_t& part : reader.metadata().parts)
    {
        if (part.message_count == 0)
        {
            continue;
        }

        // Through the spec validator rather than mcap's reader.
        //
        // This case originally opened each part with mcap::McapReader and
        // compared its channel table by hand -- which was both weaker (it only
        // looked at channels and schemas) and circular (it asked the same
        // library that wrote the file whether the file was right).
        // bag::validateMcapFile walks the raw bytes against the spec and checks
        // everything, so this is now a strictly stronger assertion with one
        // fewer dependency.
        const bag::ValidationReport report =
            bag::validateMcapFile((std::filesystem::path(dir.str()) / part.path).string());

        if (!report.ok())
        {
            for (const bag::Finding& finding : report.findings)
            {
                if (finding.severity == bag::Finding::Severity::Error)
                {
                    std::fprintf(stderr, "  %s: %s\n", part.path.c_str(),
                                 finding.message.c_str());
                }
            }
            all_consistent = false;
        }
    }

    expect(all_consistent,
           "every rolled part is valid MCAP in its own right -- summary and data agree, and "
           "the indexes point where they claim");
}

// -------------------------------------------------------------------- seeking

void testSeeking()
{
    const TempDir dir("seek");
    constexpr int kCount = 500;

    {
        bag::WriterOptions options;
        options.name = "seek";
        options.chunk_bytes = 8 * 1024;
        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < kCount; ++i)
        {
            writer.write("vehicle/engine/rpm", "EngineRpm", payloadFor(i, 256),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the bag reads back");
    if (!reader.isValid())
    {
        return;
    }

    // A window in the middle.
    const std::uint64_t start = kBase + 200ull * 1'000'000ull;
    const std::uint64_t end = kBase + 299ull * 1'000'000ull;

    std::vector<std::uint64_t> times;
    reader.forEach(start, end,
                   [&](const bag::BagMessage& message)
                   {
                       times.push_back(message.log_time_ns);
                       return true;
                   });

    expect(times.size() == 100,
           "a 100-message window returns exactly 100 (" + std::to_string(times.size()) + ")");
    expect(!times.empty() && times.front() == start,
           "the first message is the one AT the start bound, not the one after it");
    expect(!times.empty() && times.back() == end,
           "the range is closed at the top -- a message stamped exactly at `end` is included");

    // A window past the end returns nothing rather than everything, which is the
    // failure an off-by-one in the range check would produce.
    std::size_t past = 0;
    reader.forEach(kBase + 10'000ull * 1'000'000ull,
                   std::numeric_limits<std::uint64_t>::max(),
                   [&](const bag::BagMessage&)
                   {
                       ++past;
                       return true;
                   });
    expect(past == 0, "a window past the end of the recording returns nothing");

    // Early exit from the callback stops the read.
    std::size_t visited = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++visited;
            return visited < 5;
        });
    expect(visited == 5, "returning false from the callback stops the read");
}

// Topics that were advertised and never published are recorded as such. This is
// the fact that only liveliness can supply, and the reason the recorder bothers
// to snapshot the advertisement set: after the fact, "produced nothing" and "was
// not running" are indistinguishable in a file that only holds messages.
void testSilentTopicsAreRecorded()
{
    const TempDir dir("silent");

    {
        bag::WriterOptions options;
        options.name = "silent";
        bag::BagWriter writer(dir.str(), options);

        writer.noteAdvertised("vehicle/engine/rpm", "EngineRpm");
        writer.noteAdvertised("vehicle/never_publishes", "VehicleSpeed");

        writer.write("vehicle/engine/rpm", "EngineRpm", payloadFor(0, 32), kBase, std::nullopt,
                     "");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the bag reads back");

    bool found_silent = false;
    bool found_active = false;
    for (const bag::bag_topic_t& topic : reader.metadata().topics)
    {
        if (topic.key == "vehicle/never_publishes")
        {
            found_silent = topic.advertised_only && topic.message_count == 0;
        }
        if (topic.key == "vehicle/engine/rpm")
        {
            found_active = !topic.advertised_only && topic.message_count == 1;
        }
    }

    expect(found_silent,
           "a topic that was advertised and never published is recorded as silent");
    expect(found_active, "a topic that did publish is not marked silent");
}

// Two publishers on one key is a real misconfiguration, and a bag that averaged
// it away would hide it.
void testMixedOriginIsRecorded()
{
    const TempDir dir("mixed");

    {
        bag::WriterOptions options;
        options.name = "mixed";
        bag::BagWriter writer(dir.str(), options);
        writer.write("t", "EngineRpm", payloadFor(0, 32), kBase, std::nullopt, "session_a");
        writer.write("t", "EngineRpm", payloadFor(1, 32), kBase + 1, std::nullopt, "session_b");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    bool found = false;
    for (const bag::bag_topic_t& topic : reader.metadata().topics)
    {
        if (topic.key == "t")
        {
            found = topic.origin_zid == "(mixed)";
        }
    }
    expect(found, "a key published by two sessions is recorded as '(mixed)'");
}

void testDroppedAreRecorded()
{
    const TempDir dir("dropped");

    {
        bag::WriterOptions options;
        options.name = "dropped";
        bag::BagWriter writer(dir.str(), options);
        writer.write("t", "EngineRpm", payloadFor(0, 32), kBase, std::nullopt, "");
        writer.noteDropped(17);
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.metadata().dropped_messages == 17, "dropped messages reach metadata.yaml");
    expect(!reader.problems().empty(),
           "and a bag that lost messages says so when it is opened");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    testRoundTrip();
    testUnstampedAreCounted();
    testSplitting();
    testRolledPartsAreSelfConsistent();
    testSeeking();
    testSilentTopicsAreRecorded();
    testMixedOriginIsRecorded();
    testDroppedAreRecorded();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
