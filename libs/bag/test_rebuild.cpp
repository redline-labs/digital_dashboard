// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rebuilding a bag's index from the parts on disk.
//
// This is the recovery path for a recording whose recorder was killed:
// metadata.yaml is written when the recorder closes and after every roll, so a
// crash leaves parts that no index describes -- and BagReader finds parts
// through metadata.yaml and nowhere else, so those parts are unreadable until a
// rebuild runs.
//
// It is therefore the code that decides whether a crashed run is recoverable at
// all, and it had no test until now. It also has one property that is easy to
// lose and impossible to notice: it must carry over the facts the FILES cannot
// state -- the drop count above all -- because a rebuild that reset those to
// zero would silently upgrade a lossy recording to a complete one.

#include "bag/metadata.h"
#include "bag/reader.h"
#include "bag/rebuild.h"
#include "bag/writer.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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

class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("redline_bag_rebuild_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
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

void writeBag(const TempDir& dir, int count, std::uint64_t max_part_bytes = 0,
              std::uint64_t dropped = 0)
{
    bag::WriterOptions options;
    options.name = "r";
    options.compression = "none";
    options.chunk_bytes = 2 * 1024;
    options.max_part_bytes = max_part_bytes;

    bag::BagWriter writer(dir.str(), options);
    for (int i = 0; i < count; ++i)
    {
        const char* keys[] = {"a/one", "a/two"};
        const char* schemas[] = {"EngineRpm", "VehicleSpeed"};
        writer.write(keys[i % 2], schemas[i % 2], payloadFor(i, 256),
                     kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
    }
    if (dropped > 0)
    {
        writer.noteDropped(dropped);
    }
    writer.close();
}

// ------------------------------------------------------------------ the cases

// A rebuild of an intact recording must reproduce what the recorder wrote.
// If it cannot manage that, it certainly cannot be trusted with a damaged one.
void testRebuildMatchesTheOriginalIndex()
{
    const TempDir dir("match");
    writeBag(dir, 300, /*max_part_bytes=*/16 * 1024);

    const auto original = bag::loadMetadata(dir.str());
    expect(original.has_value(), "the recorder wrote an index");
    if (!original)
    {
        return;
    }

    const auto rebuilt = bag::rebuildMetadata(dir.str());
    expect(rebuilt.has_value(), "a rebuild succeeds");
    if (!rebuilt)
    {
        return;
    }

    expect(rebuilt->message_count == original->message_count,
           "the rebuilt total matches (" + std::to_string(rebuilt->message_count) + " vs " +
               std::to_string(original->message_count) + ")");
    expect(rebuilt->parts.size() == original->parts.size(),
           "every part is found (" + std::to_string(rebuilt->parts.size()) + ")");
    expect(rebuilt->t_begin_ns == original->t_begin_ns, "the start time matches");
    expect(rebuilt->t_end_ns == original->t_end_ns, "the end time matches");
    expect(rebuilt->topics.size() == original->topics.size(), "every topic is found");

    bool parts_match = true;
    for (std::size_t i = 0; i < rebuilt->parts.size() && i < original->parts.size(); ++i)
    {
        if (rebuilt->parts[i].path != original->parts[i].path ||
            rebuilt->parts[i].message_count != original->parts[i].message_count ||
            rebuilt->parts[i].t_begin_ns != original->parts[i].t_begin_ns ||
            rebuilt->parts[i].t_end_ns != original->parts[i].t_end_ns)
        {
            parts_match = false;
        }
    }
    expect(parts_match, "each part's path, count and time range match");

    // Parts must come back in chronological order, because BagReader relies on
    // that to present them as one ordered stream. The rebuild sorts by
    // filename, which is only chronological because the writer zero-pads.
    bool ordered = true;
    std::uint64_t previous = 0;
    for (const bag::bag_part_t& part : rebuilt->parts)
    {
        if (part.t_begin_ns < previous)
        {
            ordered = false;
        }
        previous = part.t_begin_ns;
    }
    expect(ordered, "the rebuilt parts are in chronological order");
}

// The recovery case: no index at all.
void testRebuildRecoversARecordingWithNoIndex()
{
    const TempDir dir("noindex");
    writeBag(dir, 250, /*max_part_bytes=*/16 * 1024);

    std::filesystem::remove(dir.path() / "metadata.yaml");

    // Without an index the bag does not open at all.
    {
        bag::BagReader reader(dir.str());
        expect(!reader.isValid(), "a recording with no index does not open");
    }

    const auto rebuilt = bag::rebuildMetadata(dir.str());
    expect(rebuilt.has_value(), "a rebuild succeeds anyway");
    if (!rebuilt)
    {
        return;
    }
    expect(bag::saveMetadata(*rebuilt, dir.str()), "and can be written back");

    // And now every message is reachable again -- which is the entire point.
    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the recording opens after the rebuild");

    std::size_t seen = 0;
    bool ordered = true;
    std::uint64_t previous = 0;
    reader.forEach(
        [&](const bag::BagMessage& message)
        {
            if (message.log_time_ns < previous)
            {
                ordered = false;
            }
            previous = message.log_time_ns;
            ++seen;
            return true;
        });

    expect(seen == 250, "and every message is readable again (" + std::to_string(seen) + ")");
    expect(ordered, "in order");
}

// THE property that is easy to lose. The files cannot say how many messages the
// recorder failed to keep up with; only the old index can. A rebuild that
// dropped that would turn a lossy recording into an apparently complete one --
// and a gap in a trace reads as a publisher that stopped.
void testRebuildPreservesTheDropCount()
{
    const TempDir dir("drops");
    writeBag(dir, 100, /*max_part_bytes=*/0, /*dropped=*/42);

    const auto before = bag::loadMetadata(dir.str());
    expect(before.has_value() && before->dropped_messages == 42, "the recording records 42 drops");

    const auto rebuilt = bag::rebuildMetadata(dir.str());
    expect(rebuilt.has_value(), "the rebuild succeeds");
    if (!rebuilt)
    {
        return;
    }

    expect(rebuilt->dropped_messages == 42,
           "the rebuilt index still says 42 messages were dropped (got " +
               std::to_string(rebuilt->dropped_messages) + ")");

    // The same reasoning for synthesised publish times.
    expect(rebuilt->compression == before->compression,
           "and the codec is carried over, which the files also cannot state per-recording");
}

// A torn part. The rebuild must include it, mark it incomplete, and count what
// is readable rather than refusing the whole directory.
void testRebuildHandlesATornPart()
{
    const TempDir dir("torn");
    writeBag(dir, 400, /*max_part_bytes=*/16 * 1024);

    const auto original = bag::loadMetadata(dir.str());
    expect(original.has_value() && original->parts.size() > 1, "the recording rolled");
    if (!original || original->parts.size() < 2)
    {
        return;
    }

    // Truncate the LAST part, which is what a killed writer leaves.
    const auto last = dir.path() / original->parts.back().path;
    std::filesystem::resize_file(last, std::filesystem::file_size(last) / 2);
    std::filesystem::remove(dir.path() / "metadata.yaml");

    const auto rebuilt = bag::rebuildMetadata(dir.str());
    expect(rebuilt.has_value(), "the rebuild succeeds despite the torn part");
    if (!rebuilt)
    {
        return;
    }

    expect(rebuilt->parts.size() == original->parts.size(),
           "the torn part is still listed rather than discarded");

    bool marked_incomplete = false;
    for (const bag::bag_part_t& part : rebuilt->parts)
    {
        if (part.path == original->parts.back().path)
        {
            marked_incomplete = !part.complete;
        }
    }
    expect(marked_incomplete,
           "and is marked incomplete, which is how a reader learns the recording was cut short");

    expect(rebuilt->message_count > 0, "messages before the tear are counted");
    expect(rebuilt->message_count < original->message_count,
           "and the ones after it are not invented (" +
               std::to_string(rebuilt->message_count) + " of " +
               std::to_string(original->message_count) + ")");
}

// A rebuild cannot restore a topic that was advertised and never published --
// nothing in a file records one. It must not claim otherwise.
void testRebuildDoesNotInventSilentTopics()
{
    const TempDir dir("silent");

    {
        bag::WriterOptions options;
        options.name = "r";
        bag::BagWriter writer(dir.str(), options);
        writer.noteAdvertised("a/never", "VehicleSpeed");
        writer.write("a/one", "EngineRpm", payloadFor(0, 64), kBase, std::nullopt, "");
        writer.close();
    }

    const auto original = bag::loadMetadata(dir.str());
    expect(original.has_value() && original->topics.size() == 2,
           "the recorder knew about both topics");

    const auto rebuilt = bag::rebuildMetadata(dir.str());
    expect(rebuilt.has_value(), "the rebuild succeeds");
    if (!rebuilt)
    {
        return;
    }

    expect(rebuilt->topics.size() == 1,
           "the rebuild lists only the topic that actually published");

    bool invented = false;
    for (const bag::bag_topic_t& topic : rebuilt->topics)
    {
        if (topic.key == "a/never")
        {
            invented = true;
        }
    }
    expect(!invented,
           "the silent topic is absent rather than fabricated -- the information is genuinely "
           "gone, and `bag reindex` says so");
}

// Directories that are not recoverable at all.
void testRebuildRefusesWhatItCannot()
{
    {
        const TempDir dir("empty");
        expect(!bag::rebuildMetadata(dir.str()).has_value(),
               "a directory with no .mcap files cannot be rebuilt");
    }
    {
        const TempDir dir("nodir");
        expect(!bag::rebuildMetadata(dir.str() + "/missing").has_value(),
               "a path that is not a directory cannot be rebuilt");
    }
    {
        const TempDir dir("garbage");
        {
            std::ofstream file(dir.path() / "junk.mcap", std::ios::binary);
            file << std::string(2048, 'x');
        }
        expect(!bag::rebuildMetadata(dir.str()).has_value(),
               "a directory whose only .mcap is garbage cannot be rebuilt");
    }
}

// Rebuilding twice must be stable -- the second run should not accumulate
// anything or drift.
void testRebuildIsIdempotent()
{
    const TempDir dir("idempotent");
    writeBag(dir, 200, /*max_part_bytes=*/16 * 1024);

    const auto first = bag::rebuildMetadata(dir.str());
    expect(first.has_value(), "the first rebuild succeeds");
    if (!first)
    {
        return;
    }
    bag::saveMetadata(*first, dir.str());

    const auto second = bag::rebuildMetadata(dir.str());
    expect(second.has_value(), "the second rebuild succeeds");
    if (!second)
    {
        return;
    }

    expect(second->message_count == first->message_count, "the counts are stable");
    expect(second->parts.size() == first->parts.size(), "the part list is stable");
    expect(second->topics.size() == first->topics.size(), "the topic list is stable");
    expect(second->dropped_messages == first->dropped_messages, "the drop count is stable");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testRebuildMatchesTheOriginalIndex();
    testRebuildRecoversARecordingWithNoIndex();
    testRebuildPreservesTheDropCount();
    testRebuildHandlesATornPart();
    testRebuildDoesNotInventSilentTopics();
    testRebuildRefusesWhatItCannot();
    testRebuildIsIdempotent();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
