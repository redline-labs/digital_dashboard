// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a damaged recording does.
//
// This is NOT an edge case. The split-directory layout exists because a recorder
// gets killed -- power cut, OOM, Ctrl-C at the wrong moment, a kernel panic on
// the vehicle -- and when it does, the last part has no summary and its final
// chunk is half written. That is the EXPECTED state of a recording after the
// event you most wanted to record.
//
// So the requirement is not "handle damage gracefully" in the abstract. It is:
// a torn recording must still yield every message up to the tear, must say that
// it is torn, and must not hang or throw while doing it. A reader that refused
// the whole file because of its last few bytes would discard the run.
//
// AGENTS.md: "the test should exercise malformed input, not just the happy
// path... bad-input handling is where the latent bugs still are."

#include "bag/reader.h"
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
                ("redline_bag_damage_" + label + "_" + std::to_string(::getpid()));
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

// Writes a normal bag, then truncates a part to `fraction` of its length --
// which is what a killed writer leaves behind: a file ending mid-chunk, with no
// summary, no footer and no closing magic.
void writeThenTruncate(const TempDir& dir, int count, double fraction, std::size_t chunk_bytes)
{
    {
        bag::WriterOptions options;
        options.name = "torn";
        options.chunk_bytes = chunk_bytes;
        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < count; ++i)
        {
            writer.write("vehicle/engine/rpm", "EngineRpm", payloadFor(i, 256),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    const std::filesystem::path part = dir.path() / "torn_0000.mcap";
    const std::uintmax_t size = std::filesystem::file_size(part);
    const std::uintmax_t truncated =
        static_cast<std::uintmax_t>(static_cast<double>(size) * fraction);

    std::filesystem::resize_file(part, truncated);
}

// ------------------------------------------------------------------ the cases

// The headline case: a part cut off partway through.
void testTruncatedPart()
{
    const TempDir dir("truncated");
    constexpr int kCount = 500;
    writeThenTruncate(dir, kCount, 0.5, 4 * 1024);

    bag::BagReader reader(dir.str());

    // Still opens. metadata.yaml was written before the truncation and is
    // intact, which is exactly the situation after a crash if the recorder had
    // rolled at least once -- and the reason metadata is written by rename
    // rather than in place.
    expect(reader.isValid(), "a bag with a truncated part still opens");
    if (!reader.isValid())
    {
        return;
    }

    // Reading must terminate, must not throw, and must yield SOMETHING. Getting
    // here at all is most of the assertion: a reader that hung or aborted would
    // take the test binary with it, which ctest reports as a failure either way.
    std::size_t seen = 0;
    bool ordered = true;
    std::uint64_t previous = 0;

    const bool completed = reader.forEach(
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

    expect(completed, "reading a truncated part returns rather than failing outright");
    expect(seen > 0, "messages before the tear are still recovered (" + std::to_string(seen) +
                         ")");
    expect(seen < kCount,
           "and the ones after it are not invented (" + std::to_string(seen) + " of " +
               std::to_string(kCount) + ")");
    expect(ordered, "what is recovered is still in order");
}

// Truncated so hard there is nothing left but the header. The reader must cope
// rather than treating "zero messages" as a reason to fail.
void testSeverelyTruncatedPart()
{
    const TempDir dir("severe");
    writeThenTruncate(dir, 500, 0.01, 4 * 1024);

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "a bag whose part is almost entirely gone still opens");

    std::size_t seen = 0;
    const bool completed = reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++seen;
            return true;
        });

    expect(completed, "reading it terminates without throwing");
}

// A part listed in metadata.yaml that is not on disk -- someone deleted it, or a
// partial copy. The rest of the recording must still read.
void testMissingPart()
{
    const TempDir dir("missing");

    {
        bag::WriterOptions options;
        options.name = "missing";
        options.compression = "none";
        options.chunk_bytes = 2 * 1024;
        options.max_part_bytes = 32 * 1024;
        bag::BagWriter writer(dir.str(), options);
        for (int i = 0; i < 300; ++i)
        {
            writer.write("t", "EngineRpm", payloadFor(i, 512),
                         kBase + static_cast<std::uint64_t>(i) * 1'000'000ull, std::nullopt, "");
        }
        writer.close();
    }

    bag::BagReader before(dir.str());
    expect(before.metadata().parts.size() > 1, "the recording rolled, so there is one to delete");
    if (before.metadata().parts.size() < 2)
    {
        return;
    }

    const std::string doomed = before.metadata().parts.front().path;
    std::filesystem::remove(dir.path() / doomed);

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "a bag with a missing part still opens");
    expect(!reader.problems().empty(), "and says which part is missing");

    bool mentions_it = false;
    for (const std::string& problem : reader.problems())
    {
        if (problem.find(doomed) != std::string::npos)
        {
            mentions_it = true;
        }
    }
    expect(mentions_it, "the problem names the part rather than being generic");

    std::size_t seen = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++seen;
            return true;
        });
    expect(seen > 0, "the surviving parts still read (" + std::to_string(seen) + ")");
}

// A file that is not an MCAP at all, in a directory that claims it is one.
void testGarbagePart()
{
    const TempDir dir("garbage");

    {
        bag::WriterOptions options;
        options.name = "garbage";
        bag::BagWriter writer(dir.str(), options);
        writer.write("t", "EngineRpm", payloadFor(0, 64), kBase, std::nullopt, "");
        writer.close();
    }

    // Overwrite the part with something that is definitely not MCAP.
    {
        std::ofstream file(dir.path() / "garbage_0000.mcap",
                           std::ios::binary | std::ios::trunc);
        const std::string junk(4096, 'x');
        file << junk;
    }

    bag::BagReader reader(dir.str());
    expect(reader.isValid(), "the bag still opens -- metadata.yaml is intact");

    // The important part is that this RETURNS. Whether it reports failure or
    // simply yields nothing is a judgement call; hanging or aborting is not.
    std::size_t seen = 0;
    reader.forEach(
        [&](const bag::BagMessage&)
        {
            ++seen;
            return true;
        });
    expect(seen == 0, "no messages are invented from garbage");
}

// An empty directory, and a directory with no metadata.yaml. Both are things a
// user will type by accident.
void testNotABag()
{
    const TempDir dir("notabag");

    bag::BagReader reader(dir.str());
    expect(!reader.isValid(), "an empty directory is not a bag");

    bag::BagReader missing(dir.str() + "/does_not_exist");
    expect(!missing.isValid(), "a path that does not exist is not a bag");
}

// A metadata.yaml that is present but not parseable.
void testCorruptMetadata()
{
    const TempDir dir("corrupt");

    {
        bag::WriterOptions options;
        options.name = "corrupt";
        bag::BagWriter writer(dir.str(), options);
        writer.write("t", "EngineRpm", payloadFor(0, 64), kBase, std::nullopt, "");
        writer.close();
    }

    {
        std::ofstream file(dir.path() / "metadata.yaml", std::ios::trunc);
        file << "this: [is: not: valid: yaml\n";
    }

    bag::BagReader reader(dir.str());
    expect(!reader.isValid(), "an unparseable metadata.yaml is reported, not ignored");
}

}  // namespace

int main()
{
    // Errors are expected throughout; keep the output about the assertions.
    spdlog::set_level(spdlog::level::off);

    testTruncatedPart();
    testSeverelyTruncatedPart();
    testMissingPart();
    testGarbagePart();
    testNotABag();
    testCorruptMetadata();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
