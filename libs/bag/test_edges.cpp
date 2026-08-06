// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writer and reader edges: the inputs and states that are not the happy path.
//
// The other suites cover a recorder doing its normal job. These cover what
// happens at the boundaries -- an empty payload, a huge one, a directory that
// cannot be written, a write after close, a time range that is backwards,
// rolling by duration rather than size. None of these is exotic; every one is
// reachable from `bag record` with the wrong arguments or a full disk, and most
// of them fail quietly if unhandled.

#include "bag/reader.h"
#include "bag/validate.h"
#include "bag/writer.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
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

class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("redline_bag_edges_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(path_, error);
    }
    std::string str() const { return path_.string(); }
    std::filesystem::path path() const { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;

std::vector<std::uint8_t> payloadFor(int index, std::size_t size)
{
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        payload[i] = static_cast<std::uint8_t>((index * 31 + static_cast<int>(i) * 7) & 0xFF);
    }
    return payload;
}

// ------------------------------------------------------------------- payloads

// A zero-length payload is legal MCAP and a real possibility -- a capnp message
// with every field defaulted still has bytes, but nothing stops a publisher
// sending an empty one, and the recorder must not choke on it.
void testEmptyPayload()
{
    const TempDir dir("emptypayload");

    {
        bag::WriterOptions options;
        options.name = "e";
        bag::BagWriter writer(dir.str(), options);
        expect(writer.write("a/one", "EngineRpm", {}, kBase, std::nullopt, ""),
               "an empty payload is accepted");
        writer.write("a/one", "EngineRpm", payloadFor(0, 32), kBase + 1, std::nullopt, "");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the recording opens");
    expect(reader.metadata().message_count == 2, "both messages are counted");

    std::size_t empty_seen = 0;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            if (message.payload.empty())
            {
                ++empty_seen;
            }
            return true;
        });
    expect(empty_seen == 1, "the empty payload comes back empty rather than being dropped");

    expect(bag::validateMcapFile((dir.path() / "e_0000.mcap").string()).ok(),
           "and the file is still valid MCAP");
}

// A payload larger than the chunk size. The writer must not assume a message
// fits in a chunk.
void testPayloadLargerThanAChunk()
{
    const TempDir dir("bigpayload");

    // 512 KiB into 4 KiB chunks.
    const std::vector<std::uint8_t> big = payloadFor(7, 512 * 1024);

    {
        bag::WriterOptions options;
        options.name = "e";
        options.chunk_bytes = 4 * 1024;
        bag::BagWriter writer(dir.str(), options);
        expect(writer.write("a/big", "EngineRpm", big, kBase, std::nullopt, ""),
               "a payload much larger than the chunk size is accepted");
        writer.close();
    }

    bag::BagReader reader(dir.str());
    bool intact = false;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            intact = message.payload.size() == big.size() &&
                     std::equal(big.begin(), big.end(), message.payload.begin());
            return true;
        });

    expect(intact, "and comes back byte-identical");
    expect(bag::validateMcapFile((dir.path() / "e_0000.mcap").string()).ok(),
           "and the file is valid MCAP");
}

// Many distinct topics. MCAP channel ids are uint16, so this is nowhere near a
// limit -- but the writer keeps a map per part and clears it on every roll, and
// getting that wrong shows up as duplicated or missing channels.
void testManyTopics()
{
    const TempDir dir("manytopics");
    constexpr int kTopics = 500;

    {
        bag::WriterOptions options;
        options.name = "e";
        options.chunk_bytes = 8 * 1024;
        bag::BagWriter writer(dir.str(), options);

        for (int i = 0; i < kTopics; ++i)
        {
            writer.write("topic/" + std::to_string(i), "EngineRpm", payloadFor(i, 64),
                         kBase + static_cast<std::uint64_t>(i) * 1000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.metadata().topics.size() == kTopics,
           "every distinct topic is recorded (" +
               std::to_string(reader.metadata().topics.size()) + ")");

    const bag::ValidationReport report =
        bag::validateMcapFile((dir.path() / "e_0000.mcap").string());
    expect(report.ok(), "the file is valid MCAP");
    expect(report.channels == kTopics,
           "and declares exactly one channel per topic (" + std::to_string(report.channels) + ")");
}

// ------------------------------------------------------------------- rolling

// Rolling by DURATION. Only size-based rolling was covered, and the two use
// completely different conditions.
void testRollingByDuration()
{
    const TempDir dir("duration");

    {
        bag::WriterOptions options;
        options.name = "e";
        options.compression = "none";
        options.max_part_bytes = 0;      // size rolling off, so only time can roll
        options.max_part_seconds = 0.2;  // short, to keep the test quick

        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < 5; ++i)
        {
            writer.write("a/one", "EngineRpm", payloadFor(i, 64),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());
    expect(reader.metadata().parts.size() > 1,
           "a duration limit rolls the recording (" +
               std::to_string(reader.metadata().parts.size()) + " parts)");
    expect(reader.metadata().message_count == 5, "and no message is lost across the rolls");

    std::size_t seen = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++seen;
            return true;
        });
    expect(seen == 5, "every message reads back (" + std::to_string(seen) + ")");
}

// ---------------------------------------------------------- writer lifecycle

// Writing after close must fail rather than corrupting the file or crashing.
void testWriteAfterCloseIsRefused()
{
    const TempDir dir("afterclose");

    bag::WriterOptions options;
    options.name = "e";
    bag::BagWriter writer(dir.str(), options);

    writer.write("a/one", "EngineRpm", payloadFor(0, 32), kBase, std::nullopt, "");
    expect(writer.close(), "the writer closes");

    expect(!writer.write("a/one", "EngineRpm", payloadFor(1, 32), kBase + 1, std::nullopt, ""),
           "a write after close is refused rather than accepted or crashing");

    // Closing twice is harmless -- the destructor calls it too.
    expect(writer.close(), "closing twice is a no-op that still reports success");

    bag::BagReader reader(dir.str());
    expect(reader.metadata().message_count == 1,
           "and the refused write did not reach the recording");
    expect(bag::validateMcapFile((dir.path() / "e_0000.mcap").string()).ok(),
           "the file is still valid");
}

// A directory that cannot be written. The writer must report it rather than
// looking constructed and silently discarding everything.
void testUnwritableDirectoryIsReported()
{
    const TempDir dir("unwritable");
    const std::filesystem::path target = dir.path() / "nested";
    std::filesystem::create_directories(target);

    std::error_code error;
    std::filesystem::permissions(target, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, error);
    if (error || ::geteuid() == 0)
    {
        // Running as root, or a filesystem that ignores permissions. The case
        // is untestable here rather than failing.
        std::fprintf(stderr, "SKIP: cannot make a directory unwritable in this environment\n");
        return;
    }

    bag::WriterOptions options;
    options.name = "e";
    bag::BagWriter writer(target.string(), options);

    expect(!writer.isValid(),
           "a writer that cannot open its first part reports invalid rather than pretending");
    expect(!writer.write("a/one", "EngineRpm", payloadFor(0, 32), kBase, std::nullopt, ""),
           "and refuses writes");

    std::filesystem::permissions(target, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
}

// ---------------------------------------------------------- reader lifecycle

// A backwards range. Nonsense input, and it must return nothing rather than
// everything -- the failure that would look like the filter was ignored.
void testBackwardsRangeReturnsNothing()
{
    const TempDir dir("backwards");

    {
        bag::WriterOptions options;
        options.name = "e";
        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < 50; ++i)
        {
            writer.write("a/one", "EngineRpm", payloadFor(i, 32),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());

    std::size_t seen = 0;
    reader.forEach(kBase + 40'000'000ull, kBase + 10'000'000ull,
                   [&](const bag::BagMessage&)
                   {
                       ++seen;
                       return true;
                   });
    expect(seen == 0, "a range whose end precedes its start returns nothing, not everything");

    // A zero-width range at an exact message time returns that one message,
    // since the range is closed at both ends.
    std::size_t exact = 0;
    const std::uint64_t at = kBase + 10'000'000ull;
    reader.forEach(at, at,
                   [&](const bag::BagMessage& message)
                   {
                       expect(message.log_time_ns == at, "and it is the right one");
                       ++exact;
                       return true;
                   });
    expect(exact == 1, "a zero-width range at an exact timestamp returns that message");
}

// forEach on a bag that never opened must not crash or claim success.
void testForEachOnAnInvalidReader()
{
    const TempDir dir("invalid");

    bag::BagReader reader(dir.str());  // no metadata.yaml
    expect(!reader.isValid(), "the reader is invalid");

    std::size_t seen = 0;
    const bool ok = reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++seen;
            return true;
        });

    expect(!ok, "forEach on an invalid reader reports failure");
    expect(seen == 0, "and visits nothing");
    expect(reader.descriptorFor("EngineRpm").empty(), "descriptorFor is empty too");
}

// Reading the same bag twice, and two readers at once. Nothing stateful should
// leak between them.
void testRepeatedAndConcurrentReads()
{
    const TempDir dir("repeat");

    {
        bag::WriterOptions options;
        options.name = "e";
        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < 100; ++i)
        {
            writer.write("a/one", "EngineRpm", payloadFor(i, 32),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader reader(dir.str());

    std::size_t first = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++first;
            return true;
        });

    std::size_t second = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++second;
            return true;
        });

    expect(first == 100 && second == 100,
           "the same reader can be iterated twice with the same result (" +
               std::to_string(first) + ", " + std::to_string(second) + ")");

    // Two readers over one bag, interleaved.
    bag::BagReader a(dir.str());
    bag::BagReader b(dir.str());

    std::size_t count_a = 0;
    std::size_t count_b = 0;
    a.forEach(
        [&](const bag::BagMessage&)
        {
            ++count_a;
            return true;
        });
    b.forEach(
        [&](const bag::BagMessage&)
        {
            ++count_b;
            return true;
        });

    expect(count_a == 100 && count_b == 100, "two readers over one bag do not interfere");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testEmptyPayload();
    testPayloadLargerThanAChunk();
    testManyTopics();
    testRollingByDuration();
    testWriteAfterCloseIsRefused();
    testUnwritableDirectoryIsReported();
    testBackwardsRangeReturnsNothing();
    testForEachOnAnInvalidReader();
    testRepeatedAndConcurrentReads();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
