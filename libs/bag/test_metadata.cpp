// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bag index: metadata.yaml round trip, and what a bad one does.
//
// It matters more than an ordinary config file because it is the ONLY thing
// that knows the recording is more than one file. Lose it, or read it wrong, and
// a bag with six parts reads as whichever ones a guess happens to find -- in
// whatever order.
//
// The write-by-rename behaviour is tested for the same reason: the recorder
// rewrites this after every roll, so "crashed partway through writing the index"
// is a state that will happen, and it must not leave a truncated file where a
// good one was.

#include "bag/metadata.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
                ("redline_bag_meta_" + label + "_" + std::to_string(::getpid()));
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

bag::bag_metadata_t sample()
{
    bag::bag_metadata_t metadata;
    metadata.version = 1;
    metadata.created = "2026-08-06T10:11:12-0700";
    metadata.recorder = "redline bag test";
    metadata.compression = "zstd";
    metadata.message_count = 1234;
    metadata.t_begin_ns = 1'785'000'000'000'000'000ull;
    metadata.t_end_ns = 1'785'000'060'000'000'000ull;
    metadata.dropped_messages = 7;
    metadata.unstamped_messages = 3;

    bag::bag_part_t part;
    part.path = "drive_0000.mcap";
    part.bytes = 65536;
    part.message_count = 1000;
    part.t_begin_ns = metadata.t_begin_ns;
    part.t_end_ns = metadata.t_begin_ns + 50'000'000'000ull;
    part.complete = true;
    metadata.parts.push_back(part);

    bag::bag_part_t torn;
    torn.path = "drive_0001.mcap";
    torn.bytes = 4096;
    torn.message_count = 234;
    torn.t_begin_ns = part.t_end_ns;
    torn.t_end_ns = metadata.t_end_ns;
    torn.complete = false;
    metadata.parts.push_back(torn);

    bag::bag_topic_t topic;
    topic.key = "vehicle/engine/rpm";
    topic.schema = "EngineRpm";
    topic.message_count = 1234;
    topic.origin_zid = "abc123";
    topic.advertised_only = false;
    metadata.topics.push_back(topic);

    bag::bag_topic_t silent;
    silent.key = "vehicle/never";
    silent.schema = "VehicleSpeed";
    silent.message_count = 0;
    silent.advertised_only = true;
    metadata.topics.push_back(silent);

    return metadata;
}

// ------------------------------------------------------------------ the cases

void testRoundTrip()
{
    const TempDir dir("roundtrip");
    const bag::bag_metadata_t original = sample();

    expect(bag::saveMetadata(original, dir.str()), "metadata saves");

    const auto loaded = bag::loadMetadata(dir.str());
    expect(loaded.has_value(), "and loads back");
    if (!loaded)
    {
        return;
    }

    expect(loaded->version == original.version, "version survives");
    expect(loaded->created == original.created, "created survives");
    expect(loaded->recorder == original.recorder, "recorder survives");
    expect(loaded->compression == original.compression, "compression survives");
    expect(loaded->message_count == original.message_count, "message count survives");

    // 64-bit nanosecond timestamps are the field most likely to be mangled by a
    // serializer that goes through double: 1.785e18 does not fit a double's 53
    // bits of mantissa, so a round trip through one loses the low ~250 ns.
    expect(loaded->t_begin_ns == original.t_begin_ns,
           "t_begin survives EXACTLY -- no precision lost through a double");
    expect(loaded->t_end_ns == original.t_end_ns, "t_end survives exactly");

    expect(loaded->dropped_messages == original.dropped_messages, "dropped count survives");
    expect(loaded->unstamped_messages == original.unstamped_messages,
           "unstamped count survives");

    expect(loaded->parts.size() == 2, "both parts survive");
    if (loaded->parts.size() == 2)
    {
        expect(loaded->parts[0].path == "drive_0000.mcap", "part order is preserved");
        expect(loaded->parts[0].complete, "a complete part stays complete");
        expect(!loaded->parts[1].complete,
               "and an incomplete one stays incomplete -- this is the flag that tells a "
               "reader the recording was cut short");
        expect(loaded->parts[1].t_end_ns == original.parts[1].t_end_ns,
               "each part's time range survives exactly");
    }

    expect(loaded->topics.size() == 2, "both topics survive");
    if (loaded->topics.size() == 2)
    {
        expect(loaded->topics[1].advertised_only,
               "a topic recorded as silent stays silent -- otherwise 'produced nothing' and "
               "'was not running' become indistinguishable");
    }
}

// The rename. A crash partway through writing the index must leave the previous
// one intact, not a truncated file.
void testSaveIsAtomic()
{
    const TempDir dir("atomic");

    bag::bag_metadata_t first = sample();
    first.message_count = 111;
    expect(bag::saveMetadata(first, dir.str()), "the first save works");

    bag::bag_metadata_t second = sample();
    second.message_count = 222;
    expect(bag::saveMetadata(second, dir.str()), "the second save works");

    const auto loaded = bag::loadMetadata(dir.str());
    expect(loaded.has_value() && loaded->message_count == 222,
           "the second save replaced the first completely");

    // No temporary left behind. One that survived would eventually be mistaken
    // for part of the recording, or would just accumulate.
    expect(!std::filesystem::exists(dir.path() / "metadata.yaml.tmp"),
           "no .tmp file is left behind");
}

void testMissingAndMalformed()
{
    const TempDir dir("bad");

    expect(!bag::loadMetadata(dir.str()).has_value(),
           "a directory with no metadata.yaml loads as nullopt");

    {
        std::ofstream file(dir.path() / "metadata.yaml", std::ios::trunc);
        file << "parts: [ this is not\n  valid: yaml\n";
    }
    expect(!bag::loadMetadata(dir.str()).has_value(), "unparseable YAML loads as nullopt");

    {
        std::ofstream file(dir.path() / "metadata.yaml", std::ios::trunc);
        file << "";
    }
    expect(!bag::loadMetadata(dir.str()).has_value(), "an empty file loads as nullopt");
}

// A file written by a NEWER recorder. It must still load with the fields this
// build understands rather than being refused -- the same append-only rule the
// liveliness key spaces follow, and for the same reason: refusing what you do
// not recognise makes the first extension a breaking change.
void testNewerVersionStillLoads()
{
    const TempDir dir("newer");

    {
        std::ofstream file(dir.path() / "metadata.yaml", std::ios::trunc);
        file << "version: 99\n"
                "created: '2026-08-06T00:00:00-0700'\n"
                "recorder: 'a future recorder'\n"
                "compression: zstd\n"
                "message_count: 42\n"
                "t_begin_ns: 1785000000000000000\n"
                "t_end_ns: 1785000060000000000\n"
                "dropped_messages: 0\n"
                "unstamped_messages: 0\n"
                "parts:\n"
                "  - path: a_0000.mcap\n"
                "    bytes: 100\n"
                "    message_count: 42\n"
                "    t_begin_ns: 1785000000000000000\n"
                "    t_end_ns: 1785000060000000000\n"
                "    complete: true\n"
                "topics: []\n";
    }

    const auto loaded = bag::loadMetadata(dir.str());
    expect(loaded.has_value(), "a newer metadata.yaml still loads");
    if (!loaded)
    {
        return;
    }
    expect(loaded->version == 99, "and reports the version it was written with");
    expect(loaded->message_count == 42, "the fields this build knows are read correctly");
    expect(loaded->parts.size() == 1, "so are its parts");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testRoundTrip();
    testSaveIsAtomic();
    testMissingAndMalformed();
    testNewerVersionStillLoads();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
